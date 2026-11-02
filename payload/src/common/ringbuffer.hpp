// ringbuffer.hpp - Shared memory ring buffer v2 for camswap

#pragma once

#include <stdint.h>

namespace camswap {

// "CAMSWAP" little-endian.
static const uint64_t kRingMagic = 0x50575357414D4143ULL;
static const uint32_t kRingVersion = 2;

// Number of lanes for different buffer sizes
static const uint32_t kRingLanes = 4;

// Number of buffers per lane
static const uint32_t kRingBufs = 3;

// Maximum frame dimensions
static const uint32_t kRingMaxWidth = 1920;
static const uint32_t kRingMaxHeight = 1088;

// Supported HAL pixel formats
static const uint32_t kFormatImplDefined = 0x22;
static const uint32_t kFormatYuv420_888 = 0x23;
static const uint32_t kFormatBlob = 0x24;

// Slot flags
static const uint32_t kSlotFlagNone = 0;
static const uint32_t kSlotFlagPassthrough = (1u << 0);
static const uint32_t kSlotFlagSwapped = (1u << 1);

// Default file-backed ring path
#define CAMSWAP_DEFAULT_RING_PATH "/data/local/tmp/camswap_ring.shm"

// Per-slot metadata inside a lane
struct RingSlot {
  volatile uint64_t seq;         // published sequence number for this slot
  volatile uint64_t consumed;    // sequence number completed by daemon
  volatile uint32_t flags;       // kSlotFlag*
  volatile uint32_t format;      // HAL pixel format
  volatile uint64_t timestampNs; // CLOCK_MONOTONIC when published
};

// Lane configuration
struct RingLane {
  // Requested by payload
  volatile uint32_t reqWidth;
  volatile uint32_t reqHeight;
  volatile uint32_t reqFormat;
  volatile uint32_t reserved0;
  volatile uint64_t reqNs; // CLOCK_MONOTONIC

  // Answered by daemon
  volatile uint32_t width;
  volatile uint32_t height;
  volatile uint32_t format;
  volatile uint32_t dataOffset; // from base of mapping
  volatile uint32_t bufBytes;   // one buffer
  volatile uint32_t reserved1;
  volatile uint64_t published;  // newest buffer is (published - 1) % kRingBufs
  volatile uint64_t publishedNs;
  volatile uint64_t consumed;   // latest sequence processed by daemon
  volatile uint64_t consumedNs;

  RingSlot slots[kRingBufs];
};

// Ring header
struct RingHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t laneCount;

  // Diagnostics
  volatile uint64_t published;
  volatile uint64_t consumed;
  volatile uint64_t dropped;
  volatile uint64_t passthroughCount;

  RingLane lane[kRingLanes];
};

// Calculate frame bytes for NV12 / YUV420
inline uint32_t ring_frame_bytes(uint32_t w, uint32_t h) {
  return w * h * 3 / 2;
}

// Calculate slice bytes for a lane
inline uint32_t ring_slice_bytes() {
  return kRingBufs * ring_frame_bytes(kRingMaxWidth, kRingMaxHeight);
}

// Base offset for arena data (page-aligned)
static const uint32_t kRingArenaBase = 4096;

// Calculate total ring size
inline uint64_t ring_total_bytes() {
  return kRingArenaBase +
         static_cast<uint64_t>(kRingLanes) * ring_slice_bytes();
}

} // namespace camswap
