// color_transfer.hpp - Color transfer for camswap daemon

#pragma once

#include <stdint.h>

namespace camswap {
namespace ai {

class ColorTransfer {
public:
  ColorTransfer();
  ~ColorTransfer();

  bool init();
  void cleanup();

  bool apply(uint8_t *src, uint8_t *dst, int width, int height);

private:
  void *histogram;
};

} // namespace ai
} // namespace camswap
