# Copyright 2017 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := userlib

MODULE_SRCS := \
    $(LOCAL_DIR)/mdi.c \

MODULE_SO_NAME := mdi

MODULE_LIBS := ulib/mxio ulib/c

include make/module.mk
