// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "kernel/vm/vm_object.h"

#include "vm_priv.h"

#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <kernel/auto_lock.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_address_region.h>
#include <lib/console.h>
#include <lib/user_copy.h>
#include <new.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#define LOCAL_TRACE MAX(VM_GLOBAL_TRACE, 0)

VmObject::VmObject() {
    LTRACEF("%p\n", this);
}

VmObject::~VmObject() {
    LTRACEF("%p\n", this);
    DEBUG_ASSERT(mapping_list_.is_empty());

    // clear our magic value
    magic_ = 0;
}

void VmObject::AddMappingLocked(VmMapping* r) TA_REQ(lock_) {
    mapping_list_.push_front(r);
}

void VmObject::RemoveMappingLocked(VmMapping* r) TA_REQ(lock_) {
    mapping_list_.erase(*r);
}

static int cmd_vm_object(int argc, const cmd_args* argv, uint32_t flags) {
    if (argc < 2) {
    notenoughargs:
        printf("not enough arguments\n");
    usage:
        printf("usage:\n");
        printf("%s dump <address>\n", argv[0].str);
        printf("%s dump_pages <address>\n", argv[0].str);
        return ERR_INTERNAL;
    }

    if (!strcmp(argv[1].str, "dump")) {
        if (argc < 2)
            goto notenoughargs;

        VmObject* o = reinterpret_cast<VmObject*>(argv[2].u);

        o->Dump(0, false);
    } else if (!strcmp(argv[1].str, "dump_pages")) {
        if (argc < 2)
            goto notenoughargs;

        VmObject* o = reinterpret_cast<VmObject*>(argv[2].u);

        o->Dump(0, true);
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return NO_ERROR;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("vm_object", "vm object debug commands", &cmd_vm_object)
#endif
STATIC_COMMAND_END(vm_object);
