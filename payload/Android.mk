# Android.mk - Build file for camswap payload

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := camswap_payload
LOCAL_MODULE_TAGS := optional

# Source files
LOCAL_SRC_FILES := \
    src/common/logging.cpp \
    src/hook/hook.cpp \
    src/main.cpp

# Include paths
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/src \
    $(LOCAL_PATH)/src/common \
    $(LOCAL_PATH)/src/hook

# Compiler flags
LOCAL_CFLAGS := -Wall -Wextra -O2 -fvisibility=hidden
LOCAL_CPPFLAGS := -std=c++17 -fno-exceptions -fno-rtti

# Linker flags
LOCAL_LDFLAGS := -Wl,-z,relro -Wl,-z,now

# NDK system libraries (liblog/libdl/libandroid are not ndk-build modules)
LOCAL_LDLIBS := -llog -ldl -landroid

# ABI
LOCAL_ARM_MODE := arm
LOCAL_ARCH := arm64

# Platform
LOCAL_SDK_VERSION := 31
LOCAL_NDK_VERSION := 27

include $(BUILD_SHARED_LIBRARY)
