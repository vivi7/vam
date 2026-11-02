// color_transfer.hpp - Temporal ROI color transfer for camswap daemon

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
  void reset_temporal_state();

  // Matches RGB color statistics (mean and variance per channel) of the
  // swapped face crop (`dst_swapped_rgb`) to the real target face crop
  // (`src_target_rgb`) strictly inside the face mask (`mask_u8 > 20`),
  // smoothing mean and standard deviation over time.
  bool apply(const uint8_t *src_target_rgb, uint8_t *dst_swapped_rgb,
             const uint8_t *mask_u8, int width, int height);

private:
  bool m_has_history;
  float m_smooth_mean_src[3];
  float m_smooth_std_src[3];
  float m_smooth_mean_dst[3];
  float m_smooth_std_dst[3];
};

} // namespace ai
} // namespace camswap
