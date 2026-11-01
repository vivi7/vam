// hook.hpp - Frame hook for camswap

#pragma once

#include <stdint.h>

#include "common/control.hpp"

namespace camswap {

extern ControlBlock *g_ctrl;

// Install hook on the specified target address
bool hook_install(uintptr_t target);

// Remove hook
bool hook_remove();

// Check if format is supported
bool hook_format_supported(int32_t format);

// Run self-test
int selftest_run();

} // namespace camswap
