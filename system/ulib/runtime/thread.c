// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <runtime/thread.h>

#include <magenta/stack.h>
#include <magenta/syscalls.h>
#include <runtime/mutex.h>
#include <stddef.h>
#include <stdint.h>

// An mxr_thread_t starts its life JOINABLE.
// - If someone calls mxr_thread_join on it, it transitions to JOINED.
// - If someone calls mxr_thread_detach on it, it transitions to DETACHED.
// - When it exits, it transitions to DONE.
// No other transitions occur.
enum {
    JOINABLE,
    DETACHED,
    JOINED,
    DONE,
};

#define MXR_THREAD_MAGIC_VALID          UINT64_C(0x97c40acdb29ee45d)
#define MXR_THREAD_MAGIC_DESTROYED      UINT64_C(0x97c0acdb29ee445d)
#define MXR_THREAD_MAGIC_STILLBORN      UINT64_C(0xc70acdb29e9e445d)
#define MXR_THREAD_MAGIC_JOINED         UINT64_C(0x9c0c7db29ee445ad)
#define MXR_THREAD_MAGIC_KILLED         UINT64_C(0x9c0adb279ee44c5d)

#define CHECK_THREAD(thread)                                           \
    do {                                                               \
        if (thread == NULL || thread->magic != MXR_THREAD_MAGIC_VALID) \
            __builtin_trap();                                          \
    } while (0)

mx_status_t mxr_thread_destroy(mxr_thread_t* thread) {
    CHECK_THREAD(thread);
    thread->magic = MXR_THREAD_MAGIC_DESTROYED;
    mx_handle_t handle = thread->handle;
    thread->handle = MX_HANDLE_INVALID;
    return handle == MX_HANDLE_INVALID ? NO_ERROR : _mx_handle_close(handle);
}

static void thread_trampoline(uintptr_t ctx) {
    mxr_thread_t* thread = (mxr_thread_t*)ctx;
    CHECK_THREAD(thread);
    thread->entry(thread->arg);
    mxr_thread_exit(thread);
}

static _Noreturn void exit_joinable(mxr_thread_t* thread) {
    // A later mxr_thread_join call will complete immediately.
    // The magic stays valid for mxr_thread_join to check.
    mx_handle_t handle = thread->handle;
    thread->handle = MX_HANDLE_INVALID;
    if (_mx_handle_close(handle) != NO_ERROR)
        __builtin_trap();
    // If there were no other handles to the thread, closing the handle
    // killed us right there.  If there are other handles, exit now.
    _mx_thread_exit();
}

static _Noreturn void exit_joined(mxr_thread_t* thread) {
    // Wake the _mx_futex_wait in mxr_thread_join (below), and then
    // die.  This has to be done with the special three-in-one vDSO
    // call because as soon as the mx_futex_wake completes, the joiner
    // is free to unmap our stack out from under us.
    thread->magic = MXR_THREAD_MAGIC_JOINED;
    mx_handle_t handle = thread->handle;
    thread->handle = MX_HANDLE_INVALID;
    _mx_futex_wake_handle_close_thread_exit(&thread->state, 1, handle);
    __builtin_trap();
}

_Noreturn void mxr_thread_exit(mxr_thread_t* thread) {
    CHECK_THREAD(thread);

    int old_state = atomic_exchange_explicit(&thread->state, DONE,
                                             memory_order_release);
    switch (old_state) {
    case DETACHED:
        // Nobody cares.  Just die, alone and in the dark.
        thread->magic = MXR_THREAD_MAGIC_DESTROYED;
        // Fall through.

    case JOINABLE:
        // Nobody's watching right now, but they might care later.
        exit_joinable(thread);
        break;

    case JOINED:
        // Somebody loves us!  Or at least intends to inherit when we die.
        exit_joined(thread);
        break;
    }

    __builtin_trap();
}

_Noreturn void mxr_thread_exit_unmap_if_detached(
    mxr_thread_t* thread, mx_handle_t vmar, uintptr_t addr, size_t len) {
    CHECK_THREAD(thread);

    int old_state = atomic_exchange_explicit(&thread->state, DONE,
                                             memory_order_release);
    switch (old_state) {
    case DETACHED:
        // Don't bother touching the mxr_thread_t about to be unmapped.
        _mx_vmar_unmap_handle_close_thread_exit(vmar, addr, len,
                                                thread->handle);
        // If that returned, the unmap operation was invalid.
        break;

    case JOINABLE:
        exit_joinable(thread);
        break;

    case JOINED:
        exit_joined(thread);
        break;
    }

    __builtin_trap();
}

// Local implementation so libruntime does not depend on libc.
static size_t local_strlen(const char* s) {
    size_t len = 0;
    while (*s++ != '\0')
        ++len;
    return len;
}

static void initialize_thread(mxr_thread_t* thread,
                              mx_handle_t handle, bool detached) {
    *thread = (mxr_thread_t){
        .handle = handle,
        .state = ATOMIC_VAR_INIT(detached ? DETACHED : JOINABLE),
        .magic = (handle == MX_HANDLE_INVALID ?
                  MXR_THREAD_MAGIC_STILLBORN :
                  MXR_THREAD_MAGIC_VALID),
    };
}

mx_status_t mxr_thread_create(mx_handle_t process, const char* name,
                              bool detached, mxr_thread_t* thread) {
    initialize_thread(thread, MX_HANDLE_INVALID, detached);
    if (name == NULL)
        name = "";
    size_t name_length = local_strlen(name) + 1;
    mx_status_t status = _mx_thread_create(process, name, name_length, 0,
                                           &thread->handle);
    if (status == NO_ERROR)
        thread->magic = MXR_THREAD_MAGIC_VALID;
    return status;
}

mx_status_t mxr_thread_start(mxr_thread_t* thread, uintptr_t stack_addr, size_t stack_size, mxr_thread_entry_t entry, void* arg) {
    CHECK_THREAD(thread);

    thread->entry = entry;
    thread->arg = arg;

    // compute the starting address of the stack
    uintptr_t sp = compute_initial_stack_pointer(stack_addr, stack_size);

    // kick off the new thread
    mx_status_t status = _mx_thread_start(thread->handle,
                                          (uintptr_t)thread_trampoline, sp,
                                          (uintptr_t)thread, 0);

    if (status != NO_ERROR)
        mxr_thread_destroy(thread);
    return status;
}

mx_status_t mxr_thread_join(mxr_thread_t* thread) {
    CHECK_THREAD(thread);

    int old_state = JOINABLE;
    if (atomic_compare_exchange_strong_explicit(
            &thread->state, &old_state, JOINED,
            memory_order_acq_rel, memory_order_acquire)) {
        do {
            switch (_mx_futex_wait(&thread->state, JOINED, MX_TIME_INFINITE)) {
            case ERR_BAD_STATE:   // Never blocked because it had changed.
            case NO_ERROR:        // Woke up because it might have changed.
                old_state = atomic_load_explicit(&thread->state,
                                                 memory_order_acquire);
                break;
            default:
                __builtin_trap();
            }
        } while (old_state == JOINED);
        if (old_state != DONE)
            __builtin_trap();
        // The magic is still VALID in the kill race (see below).
        if (thread->magic != MXR_THREAD_MAGIC_JOINED &&
            thread->magic != MXR_THREAD_MAGIC_VALID)
            __builtin_trap();
    } else {
        switch (old_state) {
        case JOINED:
        case DETACHED:
            return ERR_INVALID_ARGS;
        case DONE:
            break;
        default:
            __builtin_trap();
        }
    }

    // The thread has already closed its own handle.
    thread->magic = MXR_THREAD_MAGIC_DESTROYED;
    return NO_ERROR;
}

mx_status_t mxr_thread_detach(mxr_thread_t* thread) {
    CHECK_THREAD(thread);

    int old_state = JOINABLE;
    if (!atomic_compare_exchange_strong_explicit(
            &thread->state, &old_state, DETACHED,
            memory_order_acq_rel, memory_order_relaxed)) {
        switch (old_state) {
        case DETACHED:
        case JOINED:
            return ERR_INVALID_ARGS;
        case DONE:
            return ERR_BAD_STATE;
        default:
            __builtin_trap();
        }
    }

    return NO_ERROR;
}

bool mxr_thread_detached(mxr_thread_t* thread) {
    int state = atomic_load_explicit(&thread->state, memory_order_acquire);
    return state == DETACHED;
}

mx_status_t mxr_thread_kill(mxr_thread_t* thread) {
    CHECK_THREAD(thread);

    mx_status_t status = _mx_task_kill(thread->handle);
    if (status != NO_ERROR)
        return status;

    mx_handle_t handle = thread->handle;
    thread->handle = MX_HANDLE_INVALID;

    int old_state = atomic_exchange_explicit(&thread->state, DONE,
                                             memory_order_release);
    switch (old_state) {
    case DETACHED:
        thread->magic = MXR_THREAD_MAGIC_KILLED;
        // Fall through.

    case JOINABLE:
        return _mx_handle_close(handle);

    case JOINED:
        // We're now in a race with mxr_thread_join.  It might complete
        // and free the memory before we could fetch the handle from it.
        // So we use the copy we fetched before.  In case someone is
        // blocked in mxr_thread_join, wake the futex.  Doing so is a
        // benign race: if the address is unmapped and our futex_wake
        // fails, it's OK; if the memory is reused for something else
        // and our futex_wake tickles somebody completely unrelated,
        // well, that's why futex_wait can always have spurious wakeups.
        status = _mx_handle_close(handle);
        if (status != NO_ERROR)
            (void)_mx_futex_wake(&thread->state, 1);
        return status;
    }

    __builtin_trap();
}

mx_handle_t mxr_thread_get_handle(mxr_thread_t* thread) {
    CHECK_THREAD(thread);
    return thread->handle;
}

mx_status_t mxr_thread_adopt(mx_handle_t handle, mxr_thread_t* thread) {
    initialize_thread(thread, handle, false);
    return handle == MX_HANDLE_INVALID ? ERR_BAD_HANDLE : NO_ERROR;
}
