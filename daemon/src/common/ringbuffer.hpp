// ringbuffer.hpp - Ring buffer utilities for camswap daemon

#pragma once

#include <stdint.h>

namespace camswap {

// "CAMSWAP" little-endian.
static const uint64_t kRingMagic = 0x50575357414D4143ULL;
static const uint32_t kRingVersion = 1;

// Number of lanes for different buffer sizes
static const uint32_t kRingLanes = 4;

// Number of buffers per lane
static const uint32_t kRingBufs = 3;

// Maximum frame dimensions
static const uint32_t kRingMaxWidth = 1920;
static const uint32_t kRingMaxHeight = 1088;

// Lane configuration
struct RingLane {
  // Requested by payload
  volatile uint32_t reqWidth;
  volatile uint32_t reqHeight;
  volatile uint64_t reqNs; // CLOCK_MONOTONIC

  // Answered by daemon
  volatile uint32_t width;
  volatile uint32_t height;
  volatile uint32_t dataOffset; // from base of mapping
  volatile uint32_t bufBytes;   // one buffer
  volatile uint64_t published;  // newest buffer is (published - 1) % kRingBufs
  volatile uint64_t publishedNs;
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

  RingLane lane[kRingLanes];
};

// Calculate frame bytes for YUV420
inline uint32_t ring_frame_bytes(uint32_t w, uint32_t h) {
  return w * h * 3 / 2;
}

// Calculate slice bytes for a lane
inline uint32_t ring_slice_bytes() {
  return kRingBufs * ring_frame_bytes(kRingMaxWidth, kRingMaxHeight);
}

// Calculate total ring size
inline uint64_t ring_total_bytes() {
  const uint32_t kRingArenaBase = 4096; // page-aligned
  return kRingArenaBase +
         static_cast<uint64_t>(kRingLanes) * ring_slice_bytes();
}

} // namespace camswap
