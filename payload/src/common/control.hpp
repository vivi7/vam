// control.hpp - Control block for camswap payload

#pragma once

#include <stdint.h>

namespace camswap {

// "CAMSWAPC" little-endian.
static const uint64_t kControlMagic = 0x43544C5357414D43ULL;
static const uint32_t kControlVersion = 1;

// Name passed to PR_SET_VMA_ANON_NAME
#define CAMSWAP_MARKER_NAME "camswap"
#define CAMSWAP_MARKER_MAPS "[anon:camswap]"

// Control block structure
struct ControlBlock {
  uint64_t magic;      // kControlMagic
  uint32_t version;    // kControlVersion
  uint32_t generation; // incremented each time a new copy takes over

  // 1 while a payload owns the process
  volatile uint32_t active;
  uint32_t hooks_installed; // how many hooks installed

  // Address of shutdown function
  uint64_t shutdown_fn;

  // Load bias of the active copy
  uint64_t owner_base;

  // Owning pid
  uint32_t owner_pid;

  // Shared memory ring
  uint32_t shm_fd;   // 0 = no ring
  uint64_t shm_size; // bytes
};

static_assert(sizeof(ControlBlock) == 56, "ControlBlock layout changed");

} // namespace camswap
