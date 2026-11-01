// main.cpp - Entry point for camswap payload

#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common/control.hpp"
#include "common/logging.hpp"
#include "hook/hook.hpp"

#define LOG_TAG "CamswapPayload"

extern "C" {

// Exported function called by injector
int camswap_init() {
  LOGI("camswap: initialization started");

  // Get load bias
  Dl_info info{};
  uintptr_t base = 0;
  if (dladdr(reinterpret_cast<void *>(&camswap_init), &info) &&
      info.dli_fbase) {
    base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  }
  LOGI("camswap: library base = %p", reinterpret_cast<void *>(base));

  // Find the hook target symbol
  // android::camera3::Camera3OutputStream::returnBufferCheckedLocked
  // Mangled:
  // _ZNK7android7camera318Camera3OutputStream23returnBufferCheckedLockedEPNS0_15Camera3Buffer_tE
  const char *target_symbol = "_ZNK7android7camera318Camera3OutputStream23retur"
                              "nBufferCheckedLockedEPNS0_15Camera3Buffer_tE";

  void *target_addr = dlsym(RTLD_DEFAULT, target_symbol);
  if (!target_addr) {
    LOGE("camswap: could not resolve symbol %s", target_symbol);
    return -1;
  }

  uintptr_t target = reinterpret_cast<uintptr_t>(target_addr);
  LOGI("camswap: target symbol at %p", target_addr);

  // Install hook
  if (!camswap::hook_install(target)) {
    LOGE("camswap: failed to install hook");
    return -1;
  }

  LOGI("camswap: initialization complete");
  return 0;
}

// Exported function to shutdown
int camswap_shutdown() {
  LOGI("camswap: shutdown started");

  if (!camswap::hook_remove()) {
    LOGE("camswap: failed to remove hook");
    return -1;
  }

  LOGI("camswap: shutdown complete");
  return 0;
}

// Exported function to run self-test
int camswap_selftest() { return camswap::selftest_run(); }

// Exported function to check if active
int camswap_is_active() { return camswap::g_ctrl && camswap::g_ctrl->active; }

} // extern "C"
