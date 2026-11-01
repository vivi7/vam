// hook.cpp - Frame hook implementation for camswap

#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <android/log.h>
#include <android/sharedmem.h>

#include "../common/control.hpp"
#include "../common/logging.hpp"
#include "../common/ringbuffer.hpp"

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

namespace camswap {

// Hook trampoline - assembly implementation
extern "C" void hook_trampoline();

// The actual hook function
static void (*g_original_fn)(void *) = nullptr;
static uintptr_t g_hook_addr = 0;

// ---------------------------------------------------------------------------
// control block

ControlBlock *g_ctrl = nullptr;
static bool g_owner = false;

static ControlBlock *find_control_block() {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f)
    return nullptr;

  ControlBlock *found = nullptr;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (!strstr(line, CAMSWAP_MARKER_MAPS))
      continue;

    unsigned long start = 0, end = 0;
    if (sscanf(line, "%lx-%lx", &start, &end) != 2)
      continue;
    if (end - start < sizeof(ControlBlock))
      continue;

    ControlBlock *c = reinterpret_cast<ControlBlock *>(start);
    if (c->magic == kControlMagic && c->version == kControlVersion) {
      found = c;
      break;
    }
    LOGW("control: mapping at %lx is named like ours but has magic %llx - "
         "ignoring",
         start, static_cast<unsigned long long>(c->magic));
  }
  fclose(f);
  return found;
}

static ControlBlock *create_control_block() {
  const size_t len = 4096;
  void *p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    LOGE("control: mmap failed");
    return nullptr;
  }

  if (prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, len, CAMSWAP_MARKER_NAME) !=
      0) {
    LOGE("control: PR_SET_VMA failed");
    munmap(p, len);
    return nullptr;
  }

  ControlBlock *c = static_cast<ControlBlock *>(p);
  memset(c, 0, sizeof(*c));
  c->magic = kControlMagic;
  c->version = kControlVersion;
  return c;
}

static uintptr_t self_base() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(&self_base), &info) && info.dli_fbase)
    return reinterpret_cast<uintptr_t>(info.dli_fbase);
  return 0;
}

// ---------------------------------------------------------------------------
// ring buffer

static int g_shm_fd = 0;
static void *g_shm_ptr = nullptr;
static RingHeader *g_ring = nullptr;

static bool create_ring_buffer() {
  const size_t len = ring_total_bytes();

  // Use ashmem for shared memory
  int fd = ASharedMemory_create("camswap_ring", static_cast<size_t>(len));
  if (fd < 0) {
    LOGE("ring: ashmem_create_region failed");
    return false;
  }

  void *ptr = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    LOGE("ring: mmap failed");
    close(fd);
    return false;
  }

  g_shm_fd = fd;
  g_shm_ptr = ptr;
  g_ring = static_cast<RingHeader *>(ptr);

  g_ring->magic = kRingMagic;
  g_ring->version = kRingVersion;
  g_ring->laneCount = kRingLanes;

  LOGI("ring: created at %p, size %zu", ptr, len);
  return true;
}

static void destroy_ring_buffer() {
  if (g_ring) {
    munmap(g_ring, ring_total_bytes());
    g_ring = nullptr;
  }
  if (g_shm_fd >= 0) {
    close(g_shm_fd);
    g_shm_fd = 0;
  }
  if (g_shm_ptr) {
    munmap(g_shm_ptr, ring_total_bytes());
    g_shm_ptr = nullptr;
  }
}

// ---------------------------------------------------------------------------
// hook implementation

bool hook_install(uintptr_t target) {
  LOGI("hook: installing hook at %p", reinterpret_cast<void *>(target));

  // Find or create control block
  g_ctrl = find_control_block();
  if (!g_ctrl) {
    g_ctrl = create_control_block();
    if (!g_ctrl) {
      LOGE("hook: failed to create control block");
      return false;
    }
    g_owner = true;
    LOGI("hook: created new control block");
  } else {
    g_owner = false;
    LOGI("hook: found existing control block");
  }

  // Create ring buffer if not present
  if (g_ctrl->shm_fd == 0) {
    if (!create_ring_buffer()) {
      LOGE("hook: failed to create ring buffer");
      return false;
    }
    g_ctrl->shm_fd = g_shm_fd;
    g_ctrl->shm_size = ring_total_bytes();
    LOGI("hook: ring buffer created");
  } else {
    // Reattach to existing ring
    g_shm_fd = g_ctrl->shm_fd;
    g_shm_ptr = mmap(nullptr, g_ctrl->shm_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, g_shm_fd, 0);
    if (g_shm_ptr == MAP_FAILED) {
      LOGE("hook: failed to reattach to ring buffer");
      return false;
    }
    g_ring = static_cast<RingHeader *>(g_shm_ptr);
    LOGI("hook: reattached to ring buffer");
  }

  // Store hook address in control block
  g_ctrl->owner_base = self_base();
  g_hook_addr = target;

  LOGI("hook: hook installed at %p", reinterpret_cast<void *>(target));
  return true;
}

bool hook_remove() {
  LOGI("hook: removing hook");

  // Unmap ring buffer
  destroy_ring_buffer();

  // Clear control block
  if (g_ctrl) {
    g_ctrl->active = 0;
    g_ctrl->hooks_installed = 0;
  }

  g_hook_addr = 0;
  LOGI("hook: hook removed");
  return true;
}

bool hook_format_supported(int32_t format) {
  // Support IMPLEMENTATION_DEFINED (0x22) and YUV formats
  return format == 0x22 || format == 0x23 || format == 0x24;
}

int selftest_run() {
  LOGI("selftest: running");

  // Test 1: Control block
  if (!g_ctrl || g_ctrl->magic != kControlMagic) {
    LOGE("selftest: control block invalid");
    return 1;
  }

  // Test 2: Ring buffer
  if (!g_ring || g_ring->magic != kRingMagic) {
    LOGE("selftest: ring buffer invalid");
    return 1;
  }

  // Test 3: Format support
  if (!hook_format_supported(0x22)) {
    LOGE("selftest: format 0x22 not supported");
    return 1;
  }

  LOGI("selftest: all tests passed");
  return 0;
}

} // namespace camswap
