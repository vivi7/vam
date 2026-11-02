// pipeline.hpp - AI face swap pipeline orchestrator for camswap daemon

#pragma once

#include <stdint.h>

#include "../common/ringbuffer.hpp"
#include "color_transfer.hpp"
#include "face_landmarker.hpp"
#include "face_swap.hpp"

namespace camswap {
namespace ai {

struct PipelineConfig {
  const char *device;
  const char *lens_policy;
  const char *accel_backend;
  int fps_target;
  int fps_floor;
  int rss_budget_mb;
  const char *thermal_ladder;
  const char *model_dir;
  const char *landmarker_model;
  const char *swap_model;
  const char *emap_file;
  const char *identity_path;
  int blur;
  int blur_track;
  int blur_level;
};

struct ThermalThresholds {
  int detect_rada_c; // default 38: increase detection interval
  int roi_128_c;     // default 40: switch crop to 128
  int max_480p_c;    // default 42: lanes > 480p go to passthrough
  int passthrough_c; // default 44: total passthrough
};

struct FrameProcessResult {
  bool swapped;
  bool passthrough;
  uint32_t flags;
  uint64_t elapsed_us;
};

class Pipeline {
public:
  Pipeline();
  ~Pipeline();

  // Initializes GL context, models, emap, and identity.
  // Never fails fatally on missing models/identities: stays up in passthrough.
  bool init(const PipelineConfig &cfg);
  void cleanup();

  // Processes an NV12 buffer in-place. On any uncertainty, budget overrun, or
  // thermal limit, leaves the buffer untouched and marks passthrough.
  FrameProcessResult process_slot(uint8_t *nv12_buf, uint32_t buf_bytes,
                                  uint32_t width, uint32_t height,
                                  uint32_t format, int thermal_c);

  bool is_ready_for_swap() const { return m_ready_for_swap; }
  const ThermalThresholds &thresholds() const { return m_thresholds; }

private:
  void parse_thermal_ladder(const char *ladder);
  bool load_identity_and_emap(const char *identity_path, const char *emap_path);

  PipelineConfig m_cfg;
  ThermalThresholds m_thresholds;

  FaceLandmarker m_landmarker;
  FaceSwap m_faceswap;
  ColorTransfer m_color_transfer;

  bool m_gl_ok;
  bool m_backend_supported;
  bool m_identity_loaded;
  bool m_ready_for_swap;

  // Preallocated transformed 512-float ArcFace latent vector:
  // latent = normalize(embedding * emap)
  alignas(16) float m_latent[512];
  alignas(16) float m_raw_embedding[512];
  alignas(16) float m_emap[512 * 512];

  // Fixed scratch buffers (zero allocations in frame loop)
  static const uint32_t kMaxCropSize = 512;
  uint8_t *m_crop_target_rgb;  // 512 * 512 * 3
  uint8_t *m_crop_swapped_rgb; // 512 * 512 * 3
  uint8_t *m_crop_mask;        // 512 * 512
  uint8_t *m_roi_backup_y;     // backup of ROI luma to guarantee atomic write
  uint8_t *m_roi_backup_uv;    // backup of ROI chroma

  // Tracking state
  uint64_t m_frame_index;
  uint32_t m_lost_frames;
  bool m_has_tracked_face;
  FaceBox m_smoothed_box;
};

} // namespace ai
} // namespace camswap
