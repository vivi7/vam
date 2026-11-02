// ringbuffer.cpp - Ring buffer v2 implementation for camswap daemon

#include "ringbuffer.hpp"
#include "../ai/pipeline.hpp"
#include "../common/logging.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace camswap {
namespace ring {

namespace {

uint64_t monotonic_ns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Stale lane timeout (2 seconds)
static const uint64_t kLaneStaleNs = 2ULL * 1000ULL * 1000ULL * 1000ULL;

} // namespace

RingBuffer::RingBuffer()
    : g_shm_fd(-1), g_shm_ptr(nullptr), g_ring(nullptr), m_pipeline(nullptr) {
  memset(m_ring_path, 0, sizeof(m_ring_path));
}

RingBuffer::~RingBuffer() { cleanup(); }

bool RingBuffer::init(const char *ring_path, ai::Pipeline *pipeline) {
  m_pipeline = pipeline;
  if (!ring_path || !ring_path[0]) {
    ring_path = CAMSWAP_DEFAULT_RING_PATH;
  }
  snprintf(m_ring_path, sizeof(m_ring_path), "%s", ring_path);

  const uint64_t total = ring_total_bytes();
  LOGI("ring: initializing file-backed ring v%u at %s (%llu bytes)",
       kRingVersion, m_ring_path, static_cast<unsigned long long>(total));

  // Ensure parent directory exists if under /data/local/tmp or /dev
  mode_t old_umask = umask(0);
  int fd = open(m_ring_path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
  umask(old_umask);

  if (fd < 0) {
    LOGE("ring: open(%s) failed: %s", m_ring_path, strerror(errno));
    return false;
  }

  // Allow cameraserver to attach to the shared file
  fchmod(fd, 0666);

  if (ftruncate(fd, static_cast<off_t>(total)) != 0) {
    LOGE("ring: ftruncate(%llu) failed: %s",
         static_cast<unsigned long long>(total), strerror(errno));
    close(fd);
    return false;
  }

  void *ptr = mmap(nullptr, static_cast<size_t>(total), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    LOGE("ring: mmap failed: %s", strerror(errno));
    close(fd);
    return false;
  }

  g_shm_fd = fd;
  g_shm_ptr = ptr;
  g_ring = static_cast<RingHeader *>(ptr);

  // Daemon is the owner of the ring: initialize header and lane offsets
  memset(g_ring, 0, kRingArenaBase);
  g_ring->magic = kRingMagic;
  g_ring->version = kRingVersion;
  g_ring->laneCount = kRingLanes;

  for (uint32_t i = 0; i < kRingLanes; ++i) {
    RingLane &l = g_ring->lane[i];
    l.dataOffset = kRingArenaBase + i * ring_slice_bytes();
    l.bufBytes = 0;
  }

  __atomic_thread_fence(__ATOMIC_RELEASE);
  LOGI("ring: ready (%u lanes x %u bufs, max %ux%u)", kRingLanes, kRingBufs,
       kRingMaxWidth, kRingMaxHeight);
  return true;
}

void RingBuffer::cleanup() {
  if (!g_shm_ptr && g_shm_fd < 0)
    return;

  LOGI("ring: cleaning up ring buffer");

  if (g_shm_ptr) {
    munmap(g_shm_ptr, static_cast<size_t>(ring_total_bytes()));
    g_shm_ptr = nullptr;
    g_ring = nullptr;
  }
  if (g_shm_fd >= 0) {
    close(g_shm_fd);
    g_shm_fd = -1;
  }
}

uint32_t RingBuffer::process(int thermal_c) {
  if (!g_ring || g_ring->magic != kRingMagic ||
      g_ring->version != kRingVersion) {
    return 0;
  }

  uint32_t processed_count = 0;
  const uint64_t now = monotonic_ns();
  uint8_t *base_ptr = static_cast<uint8_t *>(g_shm_ptr);
  const uint64_t total_bytes = ring_total_bytes();

  for (uint32_t i = 0; i < kRingLanes; ++i) {
    RingLane &l = g_ring->lane[i];
    const uint32_t rw = __atomic_load_n(&l.reqWidth, __ATOMIC_ACQUIRE);
    const uint32_t rh = __atomic_load_n(&l.reqHeight, __ATOMIC_ACQUIRE);
    const uint32_t rf = __atomic_load_n(&l.reqFormat, __ATOMIC_ACQUIRE);

    if (rw == 0 || rh == 0) {
      continue;
    }

    // Retire stale lanes that have not been requested for > 2s and have no
    // pending frames
    const uint64_t rns = __atomic_load_n(&l.reqNs, __ATOMIC_ACQUIRE);
    const uint64_t pub = __atomic_load_n(&l.published, __ATOMIC_ACQUIRE);
    const uint64_t con = __atomic_load_n(&l.consumed, __ATOMIC_ACQUIRE);

    if (rns > 0 && now > rns && (now - rns) > kLaneStaleNs && pub <= con) {
      l.width = 0;
      l.height = 0;
      l.bufBytes = 0;
      __atomic_store_n(&l.reqWidth, 0u, __ATOMIC_RELEASE);
      continue;
    }

    // Carve or update lane geometry when requested by payload
    if (l.width != rw || l.height != rh || l.bufBytes == 0) {
      if (rw > kRingMaxWidth || rh > kRingMaxHeight || (rw & 1u) != 0 ||
          (rh & 1u) != 0) {
        // Dimensions outside ring arena: mark passthrough and skip carving
        LOGW("ring: lane %u requested unsupported size %ux%u, passthrough", i,
             rw, rh);
        __atomic_fetch_add(&g_ring->passthroughCount, 1ULL, __ATOMIC_RELAXED);
        continue;
      }
      l.width = rw;
      l.height = rh;
      l.format = rf ? rf : kFormatImplDefined;
      l.bufBytes = ring_frame_bytes(rw, rh);
      __atomic_thread_fence(__ATOMIC_RELEASE);
      LOGI("ring: lane %u carved for %ux%u (fmt=0x%x, bufBytes=%u)", i, rw, rh,
           l.format, l.bufBytes);
    }

    if (pub <= con) {
      continue;
    }

    // Semantics: read only the newest slot with published > consumed, skip
    // older slots and increment dropped.
    const uint64_t delta = pub - con;
    if (delta > 1) {
      const uint64_t skipped = delta - 1;
      __atomic_fetch_add(&g_ring->dropped, skipped, __ATOMIC_RELAXED);
    }

    const uint32_t slot_idx = static_cast<uint32_t>((pub - 1) % kRingBufs);
    RingSlot &slot = l.slots[slot_idx];
    const uint64_t slot_offset =
        static_cast<uint64_t>(l.dataOffset) +
        static_cast<uint64_t>(slot_idx) * static_cast<uint64_t>(l.bufBytes);

    if (slot_offset + l.bufBytes > total_bytes) {
      __atomic_store_n(&slot.flags, kSlotFlagPassthrough, __ATOMIC_RELEASE);
      __atomic_store_n(&slot.consumed, pub, __ATOMIC_RELEASE);
      __atomic_store_n(&l.consumed, pub, __ATOMIC_RELEASE);
      __atomic_fetch_add(&g_ring->passthroughCount, 1ULL, __ATOMIC_RELAXED);
      continue;
    }

    uint8_t *slot_data = base_ptr + slot_offset;
    const uint32_t slot_fmt =
        slot.format != 0 ? slot.format
                         : (l.format != 0 ? l.format : kFormatImplDefined);

    uint32_t result_flags = kSlotFlagPassthrough;
    if (m_pipeline) {
      ai::FrameProcessResult res = m_pipeline->process_slot(
          slot_data, l.bufBytes, l.width, l.height, slot_fmt, thermal_c);
      result_flags = res.flags;
      if (res.passthrough) {
        __atomic_fetch_add(&g_ring->passthroughCount, 1ULL, __ATOMIC_RELAXED);
      }
    } else {
      __atomic_fetch_add(&g_ring->passthroughCount, 1ULL, __ATOMIC_RELAXED);
    }

    // Check if producer lapped this slot while we were processing
    const uint64_t pub_after = __atomic_load_n(&l.published, __ATOMIC_ACQUIRE);
    if (pub_after - pub >= kRingBufs) {
      __atomic_store_n(&slot.flags, kSlotFlagPassthrough, __ATOMIC_RELEASE);
      __atomic_fetch_add(&g_ring->dropped, 1ULL, __ATOMIC_RELAXED);
    } else {
      __atomic_store_n(&slot.flags, result_flags, __ATOMIC_RELEASE);
    }

    // Publish consumed only after slot write is complete
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&slot.consumed, pub, __ATOMIC_RELEASE);
    __atomic_store_n(&l.consumedNs, monotonic_ns(), __ATOMIC_RELEASE);
    __atomic_store_n(&l.consumed, pub, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_ring->consumed, 1ULL, __ATOMIC_RELAXED);
    processed_count++;
  }

  return processed_count;
}

bool RingBuffer::publish_synthetic_nv12(uint32_t width, uint32_t height,
                                        uint32_t format) {
  if (!g_ring || width == 0 || height == 0 || width > kRingMaxWidth ||
      height > kRingMaxHeight || (width & 1u) != 0 || (height & 1u) != 0) {
    return false;
  }

  RingLane &l = g_ring->lane[0];
  l.reqWidth = width;
  l.reqHeight = height;
  l.reqFormat = format;
  l.reqNs = monotonic_ns();
  l.width = width;
  l.height = height;
  l.format = format;
  l.bufBytes = ring_frame_bytes(width, height);

  const uint64_t next_seq = l.published + 1;
  const uint32_t slot_idx = static_cast<uint32_t>((next_seq - 1) % kRingBufs);
  const uint64_t off =
      static_cast<uint64_t>(l.dataOffset) +
      static_cast<uint64_t>(slot_idx) * static_cast<uint64_t>(l.bufBytes);

  if (off + l.bufBytes > ring_total_bytes()) {
    return false;
  }

  uint8_t *dst = static_cast<uint8_t *>(g_shm_ptr) + off;
  const uint32_t y_bytes = width * height;
  const uint32_t uv_bytes = y_bytes / 2;

  // Synthetic face-like bright oval in center on neutral background so
  // synthetic tests have deterministic luma/chroma content
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int dx = static_cast<int>(x) - static_cast<int>(width / 2);
      int dy = static_cast<int>(y) - static_cast<int>(height / 2);
      int rx = static_cast<int>(width / 5);
      int ry = static_cast<int>(height / 4);
      bool inside =
          (rx > 0 && ry > 0) && ((dx * dx) * (ry * ry) + (dy * dy) * (rx * rx) <=
                                 (rx * rx) * (ry * ry));
      dst[y * width + x] = inside ? 180 : 64;
    }
  }
  for (uint32_t i = 0; i < uv_bytes; i += 2) {
    dst[y_bytes + i] = 120;     // U
    dst[y_bytes + i + 1] = 140; // V
  }

  RingSlot &slot = l.slots[slot_idx];
  slot.seq = next_seq;
  slot.consumed = 0;
  slot.flags = kSlotFlagNone;
  slot.format = format;
  slot.timestampNs = monotonic_ns();

  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n(&l.publishedNs, slot.timestampNs, __ATOMIC_RELEASE);
  __atomic_store_n(&l.published, next_seq, __ATOMIC_RELEASE);
  __atomic_fetch_add(&g_ring->published, 1ULL, __ATOMIC_RELAXED);
  return true;
}

int RingBuffer::get_temperature() {
  // Surface Duo (SD855) exposes battery/skin temperature in millidegrees C via
  // /sys/class/power_supply/battery/temp (tenths of degree C) and
  // /sys/class/thermal/thermal_zone*/type == "battery" or "skin-therm".
  FILE *fb = fopen("/sys/class/power_supply/battery/temp", "r");
  if (fb) {
    long tenths = 0;
    if (fscanf(fb, "%ld", &tenths) == 1 && tenths > 100 && tenths < 900) {
      fclose(fb);
      return static_cast<int>(tenths / 10);
    }
    fclose(fb);
  }

  DIR *dir = opendir("/sys/class/thermal");
  if (!dir) {
    return 32;
  }

  int preferred_c = -1;
  int fallback_c = -1;
  struct dirent *entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    if (strncmp(entry->d_name, "thermal_zone", 12) != 0)
      continue;

    char type_path[256];
    char temp_path[256];
    snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type",
             entry->d_name);
    snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/%s/temp",
             entry->d_name);

    char zone_type[64] = {0};
    FILE *ft = fopen(type_path, "r");
    if (ft) {
      if (fgets(zone_type, sizeof(zone_type), ft)) {
        size_t l = strlen(zone_type);
        while (l > 0 && (zone_type[l - 1] == '\n' || zone_type[l - 1] == '\r')) {
          zone_type[--l] = '\0';
        }
      }
      fclose(ft);
    }

    FILE *f = fopen(temp_path, "r");
    if (!f)
      continue;
    long raw = 0;
    int scanned = fscanf(f, "%ld", &raw);
    fclose(f);
    if (scanned != 1 || raw <= 0)
      continue;

    int temp_c = (raw >= 1000) ? static_cast<int>(raw / 1000)
                               : static_cast<int>(raw);
    if (temp_c < 15 || temp_c > 85)
      continue;

    // Prefer battery / skin-therm / xo-therm on Surface Duo (avoid rsoc=100%)
    if (strcmp(zone_type, "battery") == 0 ||
        strcmp(zone_type, "skin-therm") == 0 ||
        strcmp(zone_type, "bms") == 0) {
      if (temp_c > preferred_c) {
        preferred_c = temp_c;
      }
    } else if (strcmp(zone_type, "soc") != 0 &&
               strcmp(zone_type, "rsoc") != 0 && fallback_c < 0) {
      fallback_c = temp_c;
    }
  }
  closedir(dir);

  if (preferred_c > 0)
    return preferred_c;
  if (fallback_c > 0)
    return fallback_c;
  return 32;
}

} // namespace ring
} // namespace camswap
