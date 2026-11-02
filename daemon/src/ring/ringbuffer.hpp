// ringbuffer.hpp - Ring buffer class for camswap daemon

#pragma once

#include "../common/ringbuffer.hpp"
#include <stdint.h>

namespace camswap {

namespace ai {
class Pipeline;
} // namespace ai

namespace ring {

class RingBuffer {
public:
  RingBuffer();
  ~RingBuffer();

  bool init(const char *ring_path = CAMSWAP_DEFAULT_RING_PATH,
            ai::Pipeline *pipeline = nullptr);
  void cleanup();

  // Process newest published slots across all lanes. Returns number of slots
  // processed in this pass.
  uint32_t process(int thermal_c);

  // Read current thermal zone temperature in degrees Celsius.
  int get_temperature();

  // Publish a synthetic NV12 frame into lane 0 for --selftest.
  bool publish_synthetic_nv12(uint32_t width, uint32_t height,
                              uint32_t format = kFormatImplDefined);

  RingHeader *header() const { return g_ring; }
  uint8_t *base() const { return static_cast<uint8_t *>(g_shm_ptr); }

private:
  int g_shm_fd;
  void *g_shm_ptr;
  RingHeader *g_ring;
  ai::Pipeline *m_pipeline;
  char m_ring_path[256];
};

} // namespace ring
} // namespace camswap
