// ringbuffer.hpp - Ring buffer class for camswap daemon

#pragma once

#include "../common/ringbuffer.hpp"
#include <stdint.h>

namespace camswap {
namespace ring {

class RingBuffer {
public:
  RingBuffer();
  ~RingBuffer();

  bool init();
  void cleanup();

  void process();
  int get_temperature();

private:
  int g_shm_fd;
  void *g_shm_ptr;
  RingHeader *g_ring;
};

} // namespace ring
} // namespace camswap
