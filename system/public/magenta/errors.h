// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#define NO_ERROR (0)

// ======= Internal failures =======
// ERR_INTERNAL: The system encountered an otherwise unspecified error
// while performing the operation.
#define ERR_INTERNAL (-1)

// ERR_NOT_SUPPORTED: The operation is not implemented, supported,
// or enabled.
#define ERR_NOT_SUPPORTED (-2)

// ERR_NO_RESOURCES: The system was not able to allocate some resource
// needed for the operation.
#define ERR_NO_RESOURCES (-5)

// ERR_NO_MEMORY: The system was not able to allocate memory needed
// for the operation.
#define ERR_NO_MEMORY (-4)

// ERR_CALL_FAILED: The second phase of mx_channel_call() did not complete
// successfully.
#define ERR_CALL_FAILED (-53)

// ======= Parameter errors =======
// ERR_INVALID_ARGS: an argument is invalid, ex. null pointer
#define ERR_INVALID_ARGS (-10)

// ERR_WRONG_TYPE: The subject of the operation is the wrong type to
// perform the operation.
// Example: Attempting a message_read on a thread handle.
#define ERR_WRONG_TYPE (-54)

// ERR_BAD_SYSCALL: The specified syscall number is invalid.
#define ERR_BAD_SYSCALL (-11)

// ERR_BAD_HANDLE: A specified handle value does not refer to a handle.
#define ERR_BAD_HANDLE (-12)

// ERR_OUT_OF_RANGE: An argument is outside the valid range for this
// operation.
#define ERR_OUT_OF_RANGE (-13)

// ERR_BUFFER_TOO_SMALL: A caller provided buffer is too small for
// this operation.
#define ERR_BUFFER_TOO_SMALL (-14)

// ======= Precondition or state errors =======
// ERR_BAD_STATE: operation failed because the current state of the
// object does not allow it, or a precondition of the operation is
// not satisfied
#define ERR_BAD_STATE (-20)

// ERR_NOT_FOUND: The requested entity is not found.
#define ERR_NOT_FOUND (-3)

// ERR_ALREADY_EXISTS: An object with the specified identifier
// already exists.
// Example: Attempting to create a file when a file already exists
// with that name.
#define ERR_ALREADY_EXISTS (-15)

// ERR_ALREADY_BOUND: The operation failed because the named entity
// is already owned or controlled by another entity. The operation
// could succeed later if the current owner releases the entity.
#define ERR_ALREADY_BOUND (-16)

// ERR_TIMED_OUT: The time limit for the operation elapsed before
// the operation completed.
#define ERR_TIMED_OUT (-23)

// deprecated: ERR_HANDLE_CLOSED: a handle being waited on was closed
// bug MG-585 tracks the cleanup.
#define ERR_HANDLE_CLOSED (-24)
// ERR_CANCELED: The in-progress operation (e.g. a wait) has been
// canceled.
#define ERR_CANCELED (-24)

// ERR_REMOTE_CLOSED: The operation failed because the remote end
// of the subject of the operation was closed.
#define ERR_REMOTE_CLOSED (-25)

// ERR_UNAVAILABLE: The subject of the operation is currently unable
// to perform the operation.
// Note: This is used when there's no direct way for the caller to
// observe when the subject will be able to perform the operation
// and should thus retry.
#define ERR_UNAVAILABLE (-26)

// ERR_SHOULD_WAIT: The operation cannot be performed currently but
// potentially could succeed if the caller waits for a prerequisite
// to be satisfied, for example waiting for a handle to be readable
// or writable.
// Example: Attempting to read from a channel that has no
// messages waiting but has an open remote will return ERR_SHOULD_WAIT.
// Attempting to read from a channel that has no messages waiting
// and has a closed remote end will return ERR_REMOTE_CLOSED.
#define ERR_SHOULD_WAIT (-27)

// ======= Permission check errors =======
// ERR_ACCESS_DENIED: The caller did not have permission to perform
// the specified operation.
#define ERR_ACCESS_DENIED (-30)

// ======= Input-output errors =======
// ERR_IO: Otherwise unspecified error occurred during I/O.
#define ERR_IO (-40)

// ERR_REFUSED: The entity the I/O operation is being performed on
// rejected the operation.
// Example: an I2C device NAK'ing a transaction or a disk controller
// rejecting an invalid command, or a stalled USB endpoint.
#define ERR_IO_REFUSED (-41)

// ERR_IO_DATA_INTEGRITY: The data in the operation failed an integrity
// check and is possibly corrupted.
// Example: CRC or Parity error.
#define ERR_IO_DATA_INTEGRITY (-42)

// ERR_IO_DATA_LOSS: The data in the operation is currently unavailable
// and may be permanently lost.
// Example: A disk block is irrecoverably damaged.
#define ERR_IO_DATA_LOSS (-43)

// Filesystem specific errors
#define ERR_BAD_PATH (-50)
#define ERR_NOT_DIR (-51)
#define ERR_NOT_FILE (-52)
#define ERR_FILE_BIG (-55)  // file exceeds FS size limits
#define ERR_NO_SPACE (-56)  // FS or device space exhausted
