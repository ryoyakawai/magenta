// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <new.h>
#include <stddef.h>

#include <kernel/vm/vm_object.h>

#include <mxtl/intrusive_single_list.h>
#include <mxtl/ref_ptr.h>
#include <mxtl/type_support.h>

namespace mxtl {

// Arena is a fast memory allocator for objects of a single size.
// Both Alloc() and Free() are always O(1) and memory always comes
// from a single contigous chunck of page-aligned memory.
//
// The control structures and data are not interleaved so it is
// more resilient to memory bugs than traditional pool allocators.
//
// The overhead per object is two pointers (16 bytes in 64-bits)

class Arena {
public:
    Arena();
    ~Arena();

    status_t Init(const char* name, size_t ob_size, size_t max_count);
    void* Alloc();
    void Free(void* addr);
    bool in_range(void* addr) const {
        return ((addr >= static_cast<void*>(d_start_)) &&
                (addr < static_cast<void*>(d_top_)));
    }

    void* start() const { return d_start_; }
    void* end() const { return d_end_; }

private:
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    struct Node : public SinglyLinkedListable<Node*> {
        explicit Node(void* s) : slot(s) { }
        void* slot;
    };

    SinglyLinkedList<Node*> free_;

    size_t ob_size_;

    // Control region pointers.
    char* c_start_;
    char* c_top_;
    // Data region pointers.
    char* d_start_;
    char* d_top_;
    char* d_end_;

    // Memory management.
    mxtl::RefPtr<VmObject> control_vmo_;
    mxtl::RefPtr<VmObject> vmo_;
};

}
