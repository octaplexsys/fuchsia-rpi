# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := userapp
MODULE_GROUP := misc

MODULE_SRCS += $(LOCAL_DIR)/main.c

MODULE_LIBS := system/ulib/fdio system/ulib/c system/ulib/zircon

MODULE_FIDL_LIBS := system/fidl/fuchsia-hardware-backlight

include make/module.mk
