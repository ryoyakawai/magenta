// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/pci.h>
#include <ddk/protocol/usb-bus.h>
#include <ddk/protocol/usb-hci.h>
#include <ddk/protocol/usb.h>

#include <hw/reg.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "xhci-device-manager.h"
#include "xhci-root-hub.h"
#include "xhci-util.h"
#include "xhci.h"

//#define TRACE 1
#include "xhci-debug.h"

#define MAX_SLOTS 255

typedef struct usb_xhci {
    xhci_t xhci;
    // the device we implement
    mx_device_t device;

    mx_device_t* bus_device;
    usb_bus_protocol_t* bus_protocol;

    pci_protocol_t* pci_proto;
    bool legacy_irq_mode;
    mx_handle_t irq_handle;
    mx_handle_t mmio_handle;
    mx_handle_t cfg_handle;
    thrd_t irq_thread;

    // used by the start thread
    mx_device_t* parent;
} usb_xhci_t;
#define xhci_to_usb_xhci(dev) containerof(dev, usb_xhci_t, xhci)
#define dev_to_usb_xhci(dev) containerof(dev, usb_xhci_t, device)

mx_status_t xhci_add_device(xhci_t* xhci, int slot_id, int hub_address, int speed) {
    usb_xhci_t* uxhci = xhci_to_usb_xhci(xhci);
    xprintf("xhci_add_new_device\n");

    if (!uxhci->bus_device || !uxhci->bus_protocol) {
        printf("no bus device in xhci_add_device\n");
        return ERR_INTERNAL;
    }

    return uxhci->bus_protocol->add_device(uxhci->bus_device, slot_id, hub_address, speed);
}

void xhci_remove_device(xhci_t* xhci, int slot_id) {
    usb_xhci_t* uxhci = xhci_to_usb_xhci(xhci);
    xprintf("xhci_remove_device %d\n", slot_id);

    if (!uxhci->bus_device || !uxhci->bus_protocol) {
        printf("no bus device in xhci_remove_device\n");
        return;
    }

    uxhci->bus_protocol->remove_device(uxhci->bus_device, slot_id);
}

static void xhci_control_complete(iotxn_t* txn, void* cookie) {
    completion_signal((completion_t*)cookie);
}

int xhci_control_request(xhci_t* xhci, uint32_t slot_id, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void* data, uint16_t length) {
    usb_xhci_t* uxhci = xhci_to_usb_xhci(xhci);

    xprintf("xhci_control_request slot_id: %d type: 0x%02X req: %d value: %d index: %d length: %d\n",
            slot_id, request_type, request, value, index, length);

    iotxn_t* txn;

    mx_status_t status = iotxn_alloc(&txn, 0, length, 0);
    if (status != NO_ERROR) return status;
    txn->protocol = MX_PROTOCOL_USB;

    usb_protocol_data_t* proto_data = iotxn_pdata(txn, usb_protocol_data_t);

    usb_setup_t* setup = &proto_data->setup;
    setup->bmRequestType = request_type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = length;
    proto_data->device_id = slot_id;
    proto_data->ep_address = 0;
    proto_data->frame = 0;

    bool out = !!((request_type & USB_DIR_MASK) == USB_DIR_OUT);
    if (length > 0 && out) {
        txn->ops->copyto(txn, data, length, 0);
    }

    completion_t completion = COMPLETION_INIT;

    txn->length = length;
    txn->complete_cb = xhci_control_complete;
    txn->cookie = &completion;
    iotxn_queue(&uxhci->device, txn);
    completion_wait(&completion, MX_TIME_INFINITE);

    status = txn->status;
    if (status == NO_ERROR) {
        status = txn->actual;

        if (length > 0 && !out) {
            txn->ops->copyfrom(txn, data, txn->actual, 0);
        }
    }
    txn->ops->release(txn);
    xprintf("xhci_control_request returning %d\n", status);
    return status;
}

mx_status_t xhci_get_descriptor(xhci_t* xhci, uint32_t slot_id, uint8_t type, uint16_t value,
                                uint16_t index, void* data, uint16_t length) {
    return xhci_control_request(xhci, slot_id, USB_DIR_IN | type | USB_RECIP_DEVICE,
                                USB_REQ_GET_DESCRIPTOR, value, index, data, length);
}

static int xhci_irq_thread(void* arg) {
    usb_xhci_t* uxhci = (usb_xhci_t*)arg;
    xprintf("xhci_irq_thread start\n");

    // xhci_start will block, so do this part here instead of in usb_xhci_bind
    xhci_start(&uxhci->xhci);

    device_add(&uxhci->device, uxhci->parent);
    uxhci->parent = NULL;

    while (1) {
        mx_status_t wait_res;

        wait_res = mx_interrupt_wait(uxhci->irq_handle);
        if (wait_res != NO_ERROR) {
            if (wait_res != ERR_HANDLE_CLOSED) {
                printf("unexpected pci_wait_interrupt failure (%d)\n", wait_res);
            }
            mx_interrupt_complete(uxhci->irq_handle);
            break;
        }

        mx_interrupt_complete(uxhci->irq_handle);
        xhci_handle_interrupt(&uxhci->xhci, uxhci->legacy_irq_mode);
    }
    xprintf("xhci_irq_thread done\n");
    return 0;
}

static void xhci_set_bus_device(mx_device_t* device, mx_device_t* busdev) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(device);
    uxhci->bus_device = busdev;
    if (busdev) {
        device_get_protocol(busdev, MX_PROTOCOL_USB_BUS, (void**)&uxhci->bus_protocol);
        // wait until bus driver has started before doing this
        xhci_queue_start_root_hubs(&uxhci->xhci);
    } else {
        uxhci->bus_protocol = NULL;
    }
}

static size_t xhci_get_max_device_count(mx_device_t* device) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(device);
    // add one to allow device IDs to be 1-based
    return uxhci->xhci.max_slots + XHCI_RH_COUNT + 1;
}

static mx_status_t xhci_enable_ep(mx_device_t* hci_device, uint32_t device_id,
                                  usb_endpoint_descriptor_t* ep_desc, bool enable) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(hci_device);
    return xhci_enable_endpoint(&uxhci->xhci, device_id, ep_desc, enable);
}

static uint64_t xhci_get_frame(mx_device_t* hci_device) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(hci_device);
    return xhci_get_current_frame(&uxhci->xhci);
}

mx_status_t xhci_config_hub(mx_device_t* hci_device, uint32_t device_id, usb_speed_t speed,
                            usb_hub_descriptor_t* descriptor) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(hci_device);
    return xhci_configure_hub(&uxhci->xhci, device_id, speed, descriptor);
}

mx_status_t xhci_hub_device_added(mx_device_t* hci_device, uint32_t hub_address, int port,
                                  usb_speed_t speed) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(hci_device);
    return xhci_enumerate_device(&uxhci->xhci, hub_address, port, speed);
}

mx_status_t xhci_hub_device_removed(mx_device_t* hci_device, uint32_t hub_address, int port) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(hci_device);
    xhci_device_disconnected(&uxhci->xhci, hub_address, port);
    return NO_ERROR;
}

mx_status_t xhci_reset_ep(mx_device_t* device, uint32_t device_id, uint8_t ep_address) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(device);
    uint8_t ep_index = xhci_endpoint_index(ep_address);
    return xhci_reset_endpoint(&uxhci->xhci, device_id, ep_index);
}

usb_hci_protocol_t xhci_hci_protocol = {
    .set_bus_device = xhci_set_bus_device,
    .get_max_device_count = xhci_get_max_device_count,
    .enable_endpoint = xhci_enable_ep,
    .get_current_frame = xhci_get_frame,
    .configure_hub = xhci_config_hub,
    .hub_device_added = xhci_hub_device_added,
    .hub_device_removed = xhci_hub_device_removed,
    .reset_endpoint = xhci_reset_ep,
};

static mx_status_t xhci_do_iotxn_queue(xhci_t* xhci, iotxn_t* txn) {
    usb_protocol_data_t* data = iotxn_pdata(txn, usb_protocol_data_t);
    int rh_index = xhci_get_root_hub_index(xhci, data->device_id);
    if (rh_index >= 0) {
        return xhci_rh_iotxn_queue(xhci, txn, rh_index);
    }
    uint32_t device_id = data->device_id;
    if (device_id > xhci->max_slots) {
        return ERR_INVALID_ARGS;
    }
    uint8_t ep_index = xhci_endpoint_index(data->ep_address);
    if (ep_index >= XHCI_NUM_EPS) {
        return ERR_INVALID_ARGS;
    }
    uint64_t frame = data->frame;
    mx_paddr_t phys_addr;
    txn->ops->physmap(txn, &phys_addr);

    usb_setup_t* setup = (ep_index == 0 ? &data->setup : NULL);
    uint8_t direction;
    if (setup) {
        direction = setup->bmRequestType & USB_ENDPOINT_DIR_MASK;
    } else {
        direction = data->ep_address & USB_ENDPOINT_DIR_MASK;
    }

    mx_status_t status = xhci_queue_transfer(xhci, device_id, setup, phys_addr, txn->length,
                               ep_index, direction, frame, txn);

    if (status == ERR_BUFFER_TOO_SMALL) {
        // add txn to deferred_txn list for later processing
        xhci_slot_t* slot = &xhci->slots[device_id];
        xhci_endpoint_t* ep = &slot->eps[ep_index];
        list_add_tail(&ep->deferred_txns, &txn->node);
    }
    return status;
}

void xhci_process_deferred_txns(xhci_t* xhci, xhci_endpoint_t* ep, bool closed) {
    list_node_t list;
    list_node_t* node;
    iotxn_t* txn;

    list_initialize(&list);

    mtx_lock(&ep->transfer_ring.mutex);
    // make a copy of deferred_txns list so we can operate on it safely outside of the mutex
    while ((node = list_remove_head(&ep->deferred_txns)) != NULL) {
        list_add_tail(&list, node);
    }
    list_initialize(&ep->deferred_txns);
    mtx_unlock(&ep->transfer_ring.mutex);

    if (closed) {
        while ((txn = list_remove_head_type(&list, iotxn_t, node)) != NULL) {
            txn->ops->complete(txn, ERR_REMOTE_CLOSED, 0);
        }
        return;
    }

    // requeue all deferred transactions
    // this will either add them to the transfer ring or put them back on deferred_txns list
    while ((txn = list_remove_head_type(&list, iotxn_t, node)) != NULL) {
        mx_status_t status = xhci_do_iotxn_queue(xhci, txn);
        if (status != NO_ERROR && status != ERR_BUFFER_TOO_SMALL) {
            txn->ops->complete(txn, status, 0);
        }
    }
}

static void xhci_iotxn_queue(mx_device_t* hci_device, iotxn_t* txn) {
    usb_xhci_t* uxhci = dev_to_usb_xhci(hci_device);
    xhci_t* xhci = &uxhci->xhci;

    mx_status_t status = xhci_do_iotxn_queue(xhci, txn);
    if (status != NO_ERROR && status != ERR_BUFFER_TOO_SMALL) {
        txn->ops->complete(txn, status, 0);
    }
}

static void xhci_unbind(mx_device_t* dev) {
    xprintf("usb_xhci_unbind\n");
    usb_xhci_t* uxhci = dev_to_usb_xhci(dev);

    if (uxhci->bus_device) {
        device_remove(uxhci->bus_device);
    }
}

static mx_status_t xhci_release(mx_device_t* device) {
    // FIXME - do something here
    return NO_ERROR;
}

static mx_protocol_device_t xhci_device_proto = {
    .iotxn_queue = xhci_iotxn_queue,
    .unbind = xhci_unbind,
    .release = xhci_release,
};

static mx_status_t usb_xhci_bind(mx_driver_t* drv, mx_device_t* dev, void** cookie) {
    mx_handle_t irq_handle = MX_HANDLE_INVALID;
    mx_handle_t mmio_handle = MX_HANDLE_INVALID;
    mx_handle_t cfg_handle = MX_HANDLE_INVALID;
    usb_xhci_t* uxhci = NULL;
    mx_status_t status;

    pci_protocol_t* pci_proto;
    if (device_get_protocol(dev, MX_PROTOCOL_PCI, (void**)&pci_proto)) {
        status = ERR_NOT_SUPPORTED;
        goto error_return;
    }

    uxhci = calloc(1, sizeof(usb_xhci_t));
    if (!uxhci) {
        status = ERR_NO_MEMORY;
        goto error_return;
    }

    status = pci_proto->claim_device(dev);
    if (status < 0) {
        printf("usb_xhci_bind claim_device failed %d\n", status);
        goto error_return;
    }

    int bar = -1;
    void* mmio;
    uint64_t mmio_len;
    /*
     * TODO(cja): according to eXtensible Host Controller Interface revision 1.1, section 5, xhci
     * should only use BARs 0 and 1. 0 for 32 bit addressing, and 0+1 for 64 bit addressing.
     */
    for (size_t i = 0; i < PCI_MAX_BAR_COUNT; i++) {
        status = pci_proto->map_mmio(dev, i, MX_CACHE_POLICY_UNCACHED_DEVICE, &mmio, &mmio_len, &mmio_handle);
        if (status == NO_ERROR) {
            bar = i;
            break;
        }
    }
    if (bar == -1) {
        printf("usb_xhci_bind could not find bar\n");
        status = ERR_INTERNAL;
        goto error_return;
    }

    // enable bus master
    status = pci_proto->enable_bus_master(dev, true);
    if (status < 0) {
        printf("usb_xhci_bind enable_bus_master failed %d\n", status);
        goto error_return;
    }

    // select our IRQ mode
    status = pci_proto->set_irq_mode(dev, MX_PCIE_IRQ_MODE_MSI, 1);
    if (status < 0) {
        mx_status_t status_legacy = pci_proto->set_irq_mode(dev, MX_PCIE_IRQ_MODE_LEGACY, 1);

        if (status_legacy < 0) {
            printf("usb_xhci_bind Failed to set IRQ mode to either MSI "
                   "(err = %d) or Legacy (err = %d)\n",
                   status, status_legacy);
            goto error_return;
        }

        uxhci->legacy_irq_mode = true;
    }

    // register for interrupts
    status = pci_proto->map_interrupt(dev, 0, &irq_handle);
    if (status != NO_ERROR) {
        printf("usb_xhci_bind map_interrupt failed %d\n", status);
        goto error_return;
    }

    uxhci->irq_handle = irq_handle;
    uxhci->mmio_handle = mmio_handle;
    uxhci->cfg_handle = cfg_handle;
    uxhci->pci_proto = pci_proto;

    // stash this here for the startup thread to call device_add() with
    uxhci->parent = dev;

    device_init(&uxhci->device, drv, "usb-xhci", &xhci_device_proto);

    status = xhci_init(&uxhci->xhci, mmio);
    if (status < 0)
        goto error_return;

    uxhci->device.protocol_id = MX_PROTOCOL_USB_HCI;
    uxhci->device.protocol_ops = &xhci_hci_protocol;

    thrd_t thread;
    thrd_create_with_name(&thread, xhci_irq_thread, uxhci, "xhci_irq_thread");
    thrd_detach(thread);

    return NO_ERROR;

error_return:
    if (uxhci)
        free(uxhci);
    if (irq_handle != MX_HANDLE_INVALID)
        mx_handle_close(irq_handle);
    if (mmio_handle != MX_HANDLE_INVALID)
        mx_handle_close(mmio_handle);
    if (cfg_handle != MX_HANDLE_INVALID)
        mx_handle_close(cfg_handle);
    return status;
}

mx_driver_t _driver_usb_xhci = {
    .ops = {
        .bind = usb_xhci_bind,
    },
};

// clang-format off
MAGENTA_DRIVER_BEGIN(_driver_usb_xhci, "usb-xhci", "magenta", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_PCI),
    BI_ABORT_IF(NE, BIND_PCI_CLASS, 0x0C),
    BI_ABORT_IF(NE, BIND_PCI_SUBCLASS, 0x03),
    BI_MATCH_IF(EQ, BIND_PCI_INTERFACE, 0x30),
MAGENTA_DRIVER_END(_driver_usb_xhci)
