// color_transfer.cpp - Temporal ROI color transfer for camswap daemon

#include "color_transfer.hpp"
#include "../common/logging.hpp"

#include <math.h>
#include <string.h>

namespace camswap {
namespace ai {

ColorTransfer::ColorTransfer() : m_has_history(false) {
  reset_temporal_state();
}

ColorTransfer::~ColorTransfer() { cleanup(); }

bool ColorTransfer::init() {
  LOGI("ai: initializing temporal ROI color transfer");
  reset_temporal_state();
  return true;
}

void ColorTransfer::cleanup() {
  LOGI("ai: cleaning up color transfer");
  reset_temporal_state();
}

void ColorTransfer::reset_temporal_state() {
  m_has_history = false;
  memset(m_smooth_mean_src, 0, sizeof(m_smooth_mean_src));
  memset(m_smooth_std_src, 0, sizeof(m_smooth_std_src));
  memset(m_smooth_mean_dst, 0, sizeof(m_smooth_mean_dst));
  memset(m_smooth_std_dst, 0, sizeof(m_smooth_std_dst));
}

bool ColorTransfer::apply(const uint8_t *src_target_rgb,
                          uint8_t *dst_swapped_rgb, const uint8_t *mask_u8,
                          int width, int height) {
  if (!src_target_rgb || !dst_swapped_rgb || !mask_u8 || width <= 0 ||
      height <= 0) {
    return false;
  }

  const int total_px = width * height;
  double sum_src[3] = {0.0, 0.0, 0.0};
  double sum_sq_src[3] = {0.0, 0.0, 0.0};
  double sum_dst[3] = {0.0, 0.0, 0.0};
  double sum_sq_dst[3] = {0.0, 0.0, 0.0};
  int count = 0;

  for (int i = 0; i < total_px; ++i) {
    if (mask_u8[i] <= 20)
      continue;
    const uint8_t *s = &src_target_rgb[i * 3];
    const uint8_t *d = &dst_swapped_rgb[i * 3];
    for (int c = 0; c < 3; ++c) {
      double sv = static_cast<double>(s[c]);
      double dv = static_cast<double>(d[c]);
      sum_src[c] += sv;
      sum_sq_src[c] += sv * sv;
      sum_dst[c] += dv;
      sum_sq_dst[c] += dv * dv;
    }
    count++;
  }

  if (count < 64) {
    return false;
  }

  float cur_mean_src[3], cur_std_src[3];
  float cur_mean_dst[3], cur_std_dst[3];
  const double inv_n = 1.0 / static_cast<double>(count);

  for (int c = 0; c < 3; ++c) {
    double ms = sum_src[c] * inv_n;
    double vs = (sum_sq_src[c] * inv_n) - (ms * ms);
    if (vs < 1.0)
      vs = 1.0;

    double md = sum_dst[c] * inv_n;
    double vd = (sum_sq_dst[c] * inv_n) - (md * md);
    if (vd < 1.0)
      vd = 1.0;

    cur_mean_src[c] = static_cast<float>(ms);
    cur_std_src[c] = static_cast<float>(sqrt(vs));
    cur_mean_dst[c] = static_cast<float>(md);
    cur_std_dst[c] = static_cast<float>(sqrt(vd));
  }

  // Temporal exponential moving average (alpha = 0.25) to avoid frame-to-frame
  // color flicker during head motion or lighting shifts.
  const float alpha = 0.25f;
  if (!m_has_history) {
    for (int c = 0; c < 3; ++c) {
      m_smooth_mean_src[c] = cur_mean_src[c];
      m_smooth_std_src[c] = cur_std_src[c];
      m_smooth_mean_dst[c] = cur_mean_dst[c];
      m_smooth_std_dst[c] = cur_std_dst[c];
    }
    m_has_history = true;
  } else {
    for (int c = 0; c < 3; ++c) {
      m_smooth_mean_src[c] =
          alpha * cur_mean_src[c] + (1.0f - alpha) * m_smooth_mean_src[c];
      m_smooth_std_src[c] =
          alpha * cur_std_src[c] + (1.0f - alpha) * m_smooth_std_src[c];
      m_smooth_mean_dst[c] =
          alpha * cur_mean_dst[c] + (1.0f - alpha) * m_smooth_mean_dst[c];
      m_smooth_std_dst[c] =
          alpha * cur_std_dst[c] + (1.0f - alpha) * m_smooth_std_dst[c];
    }
  }

  float scale[3];
  float shift[3];
  for (int c = 0; c < 3; ++c) {
    float ratio = m_smooth_std_src[c] /
                  (m_smooth_std_dst[c] > 1e-3f ? m_smooth_std_dst[c] : 1.0f);
    // Clamp gain to [0.65, 1.45] to prevent extreme color blowouts
    if (ratio < 0.65f)
      ratio = 0.65f;
    if (ratio > 1.45f)
      ratio = 1.45f;
    scale[c] = ratio;
    shift[c] = m_smooth_mean_src[c] - m_smooth_mean_dst[c] * ratio;
  }

  for (int i = 0; i < total_px; ++i) {
    if (mask_u8[i] == 0)
      continue;
    uint8_t *d = &dst_swapped_rgb[i * 3];
    for (int c = 0; c < 3; ++c) {
      float v = static_cast<float>(d[c]) * scale[c] + shift[c];
      int iv = static_cast<int>(v + 0.5f);
      d[c] = static_cast<uint8_t>(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
    }
  }

  return true;
}

} // namespace ai
} // namespace camswap
