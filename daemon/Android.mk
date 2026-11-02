# Android.mk - Build file for camswap daemon

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := camswap_daemon
LOCAL_MODULE_TAGS := optional

# Source files
LOCAL_SRC_FILES := \
    src/main.cpp \
    src/ring/ringbuffer.cpp \
    src/ai/pipeline.cpp \
    src/ai/face_landmarker.cpp \
    src/ai/face_swap.cpp \
    src/ai/color_transfer.cpp \
    src/gl/context.cpp

# Include paths
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/src \
    $(LOCAL_PATH)/src/common \
    $(LOCAL_PATH)/src/ring \
    $(LOCAL_PATH)/src/ai \
    $(LOCAL_PATH)/src/gl \
    $(LOCAL_PATH)/src/utils

# Compiler flags
LOCAL_CFLAGS := -Wall -Wextra -O2 -fvisibility=hidden
LOCAL_CPPFLAGS := -std=c++17 -fno-exceptions -fno-rtti

# Linker flags
LOCAL_LDFLAGS := -Wl,-z,relro -Wl,-z,now

# NDK system libraries
LOCAL_LDLIBS := -llog -ldl -lEGL -lGLESv2 -lmediandk

# ABI
LOCAL_ARM_MODE := arm
LOCAL_ARCH := arm64

# Platform
LOCAL_SDK_VERSION := 31
LOCAL_NDK_VERSION := 27

include $(BUILD_EXECUTABLE)
