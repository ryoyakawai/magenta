// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <magenta/types.h>

#include "xhci.h"

typedef struct xhci xhci_t;

typedef void (*xhci_transfer_complete_cb)(mx_status_t result, void* data);

mx_status_t xhci_queue_transfer(xhci_t* xhci, uint32_t slot_id, usb_setup_t* setup, mx_paddr_t data,
                                uint16_t length, int ep, int direction, uint64_t frame,
                                iotxn_t* txn);
void xhci_cancel_transfers(xhci_t* xhci, xhci_transfer_ring_t* ring);
void xhci_handle_transfer_event(xhci_t* xhci, xhci_trb_t* trb);

mx_status_t xhci_reset_endpoint(xhci_t* xhci, uint32_t slot_id, uint32_t endpoint);
