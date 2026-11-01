// logging.hpp - Logging utilities for camswap payload

#pragma once

#include <android/log.h>
#include <cstdio>
#include <cstdlib>

#define LOG_TAG "CamswapPayload"

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      LOGE("CHECK failed: %s at %s:%d", #condition, __FILE__, __LINE__);       \
      abort();                                                                 \
    }                                                                          \
  } while (0)

#define CHECK_NOTNULL(ptr)                                                     \
  do {                                                                         \
    if ((ptr) == nullptr) {                                                    \
      LOGE("CHECK_NOTNULL failed: %s at %s:%d", #ptr, __FILE__, __LINE__);     \
      abort();                                                                 \
    }                                                                          \
  } while (0)
