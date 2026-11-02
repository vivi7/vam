// pipeline.cpp - AI face swap pipeline orchestrator for camswap daemon

#include "pipeline.hpp"
#include "../common/logging.hpp"
#include "../gl/context.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace camswap {
namespace ai {

namespace {

uint64_t monotonic_us() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

static const uint32_t kMaxLostFramesBeforePassthrough = 15;
static const uint32_t kDefaultDetectInterval = 8;

} // namespace

Pipeline::Pipeline()
    : m_gl_ok(false), m_backend_supported(false), m_identity_loaded(false),
      m_ready_for_swap(false), m_crop_target_rgb(nullptr),
      m_crop_swapped_rgb(nullptr), m_crop_mask(nullptr),
      m_roi_backup_y(nullptr), m_roi_backup_uv(nullptr), m_frame_index(0),
      m_lost_frames(0), m_has_tracked_face(false) {
  memset(&m_cfg, 0, sizeof(m_cfg));
  m_thresholds = {38, 40, 42, 44};
  memset(m_latent, 0, sizeof(m_latent));
  memset(m_raw_embedding, 0, sizeof(m_raw_embedding));
  memset(m_emap, 0, sizeof(m_emap));
  memset(&m_smoothed_box, 0, sizeof(m_smoothed_box));
}

Pipeline::~Pipeline() { cleanup(); }

void Pipeline::parse_thermal_ladder(const char *ladder) {
  m_thresholds = {38, 40, 42, 44};
  if (!ladder || !ladder[0]) {
    return;
  }

  char buf[256];
  snprintf(buf, sizeof(buf), "%s", ladder);
  char *saveptr = nullptr;
  char *tok = strtok_r(buf, ",", &saveptr);
  while (tok) {
    char *colon = strchr(tok, ':');
    if (colon) {
      *colon = '\0';
      int temp = atoi(tok);
      const char *action = colon + 1;
      if (temp > 0) {
        if (strcmp(action, "detect_rada") == 0) {
          m_thresholds.detect_rada_c = temp;
        } else if (strcmp(action, "roi_128") == 0) {
          m_thresholds.roi_128_c = temp;
        } else if (strcmp(action, "480p") == 0) {
          m_thresholds.max_480p_c = temp;
        } else if (strcmp(action, "passthrough") == 0) {
          m_thresholds.passthrough_c = temp;
        }
      }
    }
    tok = strtok_r(nullptr, ",", &saveptr);
  }

  LOGI("pipeline: thermal ladder parsed -> detect_rada=%dC, roi_128=%dC, "
       "480p=%dC, passthrough=%dC",
       m_thresholds.detect_rada_c, m_thresholds.roi_128_c,
       m_thresholds.max_480p_c, m_thresholds.passthrough_c);
}

bool Pipeline::load_identity_and_emap(const char *identity_path,
                                      const char *emap_path) {
  m_identity_loaded = false;
  if (!identity_path || !identity_path[0] || !emap_path || !emap_path[0]) {
    LOGW("pipeline: identity_path or emap_path empty; passthrough active");
    return false;
  }

  // Load 512 float32 ArcFace embedding from identities/default.bin
  FILE *fi = fopen(identity_path, "rb");
  if (!fi) {
    LOGW("pipeline: identity file not found (%s); passthrough active",
         identity_path);
    return false;
  }

  size_t n_emb = fread(m_raw_embedding, sizeof(float), 512, fi);
  fclose(fi);
  if (n_emb != 512) {
    LOGW("pipeline: identity file %s invalid size (read %zu/512 floats); "
         "passthrough active",
         identity_path, n_emb);
    return false;
  }

  // Load emap.bin: 8-byte header (int32 rows=512, int32 cols=512) + 512x512
  // float32 matrix (matches refs/android-face-fusion/ARCHITECTURE.md)
  FILE *fe = fopen(emap_path, "rb");
  if (!fe) {
    LOGW("pipeline: emap file not found (%s); passthrough active", emap_path);
    return false;
  }

  int32_t dims[2] = {0, 0};
  if (fread(dims, sizeof(int32_t), 2, fe) != 2 || dims[0] != 512 ||
      dims[1] != 512) {
    LOGW("pipeline: emap file %s invalid header (%dx%d); passthrough active",
         emap_path, dims[0], dims[1]);
    fclose(fe);
    return false;
  }

  size_t n_emap = fread(m_emap, sizeof(float), 512 * 512, fe);
  fclose(fe);
  if (n_emap != 512 * 512) {
    LOGW("pipeline: emap file %s truncated (%zu floats); passthrough active",
         emap_path, n_emap);
    return false;
  }

  // Compute latent = normalize(embedding · emap) once at init
  double norm_sq = 0.0;
  for (int i = 0; i < 512; ++i) {
    double acc = 0.0;
    for (int j = 0; j < 512; ++j) {
      acc += static_cast<double>(m_raw_embedding[j]) *
             static_cast<double>(m_emap[j * 512 + i]);
    }
    m_latent[i] = static_cast<float>(acc);
    norm_sq += acc * acc;
  }

  if (norm_sq < 1e-12 || isnan(norm_sq) || isinf(norm_sq)) {
    LOGW("pipeline: transformed latent has zero/invalid norm; passthrough");
    return false;
  }

  float inv_norm = static_cast<float>(1.0 / sqrt(norm_sq));
  for (int i = 0; i < 512; ++i) {
    m_latent[i] *= inv_norm;
  }

  m_identity_loaded = true;
  LOGI("pipeline: loaded identity (%s) and emap (%s), latent normalized",
       identity_path, emap_path);
  return true;
}

bool Pipeline::init(const PipelineConfig &cfg) {
  m_cfg = cfg;
  parse_thermal_ladder(cfg.thermal_ladder);

  // Allocate fixed scratch buffers once (zero per-frame allocations)
  const size_t crop_rgb_bytes =
      static_cast<size_t>(kMaxCropSize) * kMaxCropSize * 3;
  const size_t crop_mask_bytes =
      static_cast<size_t>(kMaxCropSize) * kMaxCropSize;
  const size_t roi_backup_bytes = 640 * 640;

  m_crop_target_rgb = static_cast<uint8_t *>(malloc(crop_rgb_bytes));
  m_crop_swapped_rgb = static_cast<uint8_t *>(malloc(crop_rgb_bytes));
  m_crop_mask = static_cast<uint8_t *>(malloc(crop_mask_bytes));
  m_roi_backup_y = static_cast<uint8_t *>(malloc(roi_backup_bytes));
  m_roi_backup_uv = static_cast<uint8_t *>(malloc(roi_backup_bytes / 2));

  if (!m_crop_target_rgb || !m_crop_swapped_rgb || !m_crop_mask ||
      !m_roi_backup_y || !m_roi_backup_uv) {
    LOGE("pipeline: failed to allocate fixed scratch buffers");
    return false;
  }

  // Verify accel_backend: gpu_gl selects the GL pipeline; any unknown value
  // forces passthrough on every frame.
  m_backend_supported =
      (cfg.accel_backend && strcmp(cfg.accel_backend, "gpu_gl") == 0);
  if (!m_backend_supported) {
    LOGW("pipeline: unknown accel_backend='%s' (expected 'gpu_gl'); forcing "
         "passthrough",
         cfg.accel_backend ? cfg.accel_backend : "(null)");
  }

  m_gl_ok = gl::init_context();
  m_color_transfer.init();

  char landmarker_path[512] = {0};
  char scrfd_fallback_path[512] = {0};
  char swap_path[512] = {0};
  char emap_path[512] = {0};

  const char *mdir =
      (cfg.model_dir && cfg.model_dir[0])
          ? cfg.model_dir
          : "/data/adb/modules/camswap/system/etc/camswap/models";

  if (cfg.landmarker_model && cfg.landmarker_model[0] == '/') {
    snprintf(landmarker_path, sizeof(landmarker_path), "%s",
             cfg.landmarker_model);
  } else {
    snprintf(landmarker_path, sizeof(landmarker_path), "%s/%s", mdir,
             (cfg.landmarker_model && cfg.landmarker_model[0])
                 ? cfg.landmarker_model
                 : "face_landmarker.task");
  }
  snprintf(scrfd_fallback_path, sizeof(scrfd_fallback_path), "%s/det_10g.onnx",
           mdir);

  if (cfg.swap_model && cfg.swap_model[0] == '/') {
    snprintf(swap_path, sizeof(swap_path), "%s", cfg.swap_model);
  } else {
    snprintf(swap_path, sizeof(swap_path), "%s/%s", mdir,
             (cfg.swap_model && cfg.swap_model[0])
                 ? cfg.swap_model
                 : "inswapper_512_live_int8.onnx");
  }

  if (cfg.emap_file && cfg.emap_file[0] == '/') {
    snprintf(emap_path, sizeof(emap_path), "%s", cfg.emap_file);
  } else {
    snprintf(emap_path, sizeof(emap_path), "%s/%s", mdir,
             (cfg.emap_file && cfg.emap_file[0]) ? cfg.emap_file : "emap.bin");
  }

  m_landmarker.init(landmarker_path, scrfd_fallback_path);
  m_faceswap.init(swap_path);
  load_identity_and_emap(cfg.identity_path, emap_path);

  m_ready_for_swap = m_backend_supported && m_gl_ok &&
                     m_landmarker.model_ready() && m_faceswap.model_ready() &&
                     m_identity_loaded;

  LOGI("pipeline: initialized (ready_for_swap=%d, backend_ok=%d, gl_ok=%d, "
       "landmarker=%d, swap=%d, identity=%d)",
       m_ready_for_swap ? 1 : 0, m_backend_supported ? 1 : 0, m_gl_ok ? 1 : 0,
       m_landmarker.model_ready() ? 1 : 0, m_faceswap.model_ready() ? 1 : 0,
       m_identity_loaded ? 1 : 0);
  return true;
}

void Pipeline::cleanup() {
  m_faceswap.cleanup();
  m_landmarker.cleanup();
  m_color_transfer.cleanup();
  gl::cleanup_context();

  if (m_crop_target_rgb) {
    free(m_crop_target_rgb);
    m_crop_target_rgb = nullptr;
  }
  if (m_crop_swapped_rgb) {
    free(m_crop_swapped_rgb);
    m_crop_swapped_rgb = nullptr;
  }
  if (m_crop_mask) {
    free(m_crop_mask);
    m_crop_mask = nullptr;
  }
  if (m_roi_backup_y) {
    free(m_roi_backup_y);
    m_roi_backup_y = nullptr;
  }
  if (m_roi_backup_uv) {
    free(m_roi_backup_uv);
    m_roi_backup_uv = nullptr;
  }

  m_ready_for_swap = false;
}

FrameProcessResult Pipeline::process_slot(uint8_t *nv12_buf, uint32_t buf_bytes,
                                          uint32_t width, uint32_t height,
                                          uint32_t format, int thermal_c) {
  const uint64_t t_start = monotonic_us();
  FrameProcessResult res{};
  res.swapped = false;
  res.passthrough = true;
  res.flags = kSlotFlagPassthrough;

  // 1. Validate format and buffer layout: only preview NV12/YUV is processed
  // here. BLOB (0x24) or any unrecognized layout -> passthrough, zero writes.
  if (!nv12_buf || width == 0 || height == 0 || (width & 1u) != 0 ||
      (height & 1u) != 0 || buf_bytes < ring_frame_bytes(width, height)) {
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }

  if (format == kFormatBlob ||
      (format != kFormatImplDefined && format != kFormatYuv420_888)) {
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }

  // 2. Thermal ladder checks:
  // - >= 44C: total passthrough (daemon stays alive, never exits)
  // - >= 42C: any lane above 480p goes to passthrough
  if (thermal_c >= m_thresholds.passthrough_c) {
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }
  if (thermal_c >= m_thresholds.max_480p_c && height > 480) {
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }

  // 3. Check pipeline readiness (models, identity, emap, backend)
  if (!m_ready_for_swap) {
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }

  // 4. Select detection cadence and crop size from thermal ladder:
  // - < 38C: detect every 8 frames, crop 512
  // - >= 38C (detect_rada): detect every 12 frames
  // - >= 40C (roi_128): crop 128
  const uint32_t detect_interval =
      (thermal_c >= m_thresholds.detect_rada_c) ? 12u : kDefaultDetectInterval;
  const uint32_t crop_size =
      (thermal_c >= m_thresholds.roi_128_c) ? 128u : 512u;

  const uint64_t frame_no = m_frame_index++;
  const bool run_full_detect =
      (!m_has_tracked_face) || ((frame_no % detect_interval) == 0);

  FaceBox current_box{};
  bool got_landmarks = false;

  if (run_full_detect) {
    got_landmarks = m_landmarker.detect_landmarks(
        nv12_buf, static_cast<int>(width), static_cast<int>(height),
        &current_box);
  } else {
    got_landmarks = m_landmarker.track_landmarks(
        nv12_buf, static_cast<int>(width), static_cast<int>(height),
        m_smoothed_box, &current_box);
  }

  if (got_landmarks) {
    if (!m_has_tracked_face) {
      m_smoothed_box = current_box;
      m_has_tracked_face = true;
    } else {
      // Exponential smoothing of bounding box and 5-point landmarks
      const float alpha = 0.45f;
      m_smoothed_box.x1 =
          alpha * current_box.x1 + (1.0f - alpha) * m_smoothed_box.x1;
      m_smoothed_box.y1 =
          alpha * current_box.y1 + (1.0f - alpha) * m_smoothed_box.y1;
      m_smoothed_box.x2 =
          alpha * current_box.x2 + (1.0f - alpha) * m_smoothed_box.x2;
      m_smoothed_box.y2 =
          alpha * current_box.y2 + (1.0f - alpha) * m_smoothed_box.y2;
      m_smoothed_box.score = current_box.score;
      for (int i = 0; i < 10; ++i) {
        m_smoothed_box.kps[i] =
            alpha * current_box.kps[i] + (1.0f - alpha) * m_smoothed_box.kps[i];
      }
    }
    m_lost_frames = 0;
  } else {
    // Tracking lost: reuse last valid landmarks for at most 15 frames, then
    // pure passthrough (never hybrid frames).
    if (!m_has_tracked_face || m_lost_frames >= kMaxLostFramesBeforePassthrough) {
      m_has_tracked_face = false;
      m_color_transfer.reset_temporal_state();
      res.elapsed_us = monotonic_us() - t_start;
      return res;
    }
    m_lost_frames++;
  }

  // 5. Run aligned crop + INSwapper int8 inference
  float fwd_affine[6] = {0};
  uint32_t roi_x0 = 0, roi_y0 = 0, roi_w = 0, roi_h = 0;
  if (!m_faceswap.swap_face(nv12_buf, width, height, m_smoothed_box.kps,
                            m_latent, crop_size, m_crop_target_rgb,
                            m_crop_swapped_rgb, m_crop_mask, fwd_affine,
                            &roi_x0, &roi_y0, &roi_w, &roi_h)) {
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }

  // Frame budget check before mutating the slot: if inference exceeded the
  // frame budget (1000ms / fps_floor), abort write and mark passthrough so a
  // partial/late frame is never emitted.
  const int fps_floor = (m_cfg.fps_floor > 0) ? m_cfg.fps_floor : 15;
  const uint64_t budget_us =
      1000000ULL / static_cast<uint64_t>(fps_floor);
  if ((monotonic_us() - t_start) > budget_us) {
    LOGW("pipeline: frame budget exceeded (%llu us > %llu us), passthrough",
         static_cast<unsigned long long>(monotonic_us() - t_start),
         static_cast<unsigned long long>(budget_us));
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }

  // 6. Temporal color transfer strictly on the aligned ROI patch
  m_color_transfer.apply(m_crop_target_rgb, m_crop_swapped_rgb, m_crop_mask,
                         static_cast<int>(crop_size),
                         static_cast<int>(crop_size));

  // 7. Paste ROI back into the NV12 slot via GL compositor
  if (!gl::paste_roi_nv12(nv12_buf, width, height, m_crop_swapped_rgb,
                          m_crop_mask, crop_size, fwd_affine, roi_x0, roi_y0,
                          roi_w, roi_h)) {
    res.elapsed_us = monotonic_us() - t_start;
    return res;
  }

  res.swapped = true;
  res.passthrough = false;
  res.flags = kSlotFlagSwapped;
  res.elapsed_us = monotonic_us() - t_start;
  return res;
}

} // namespace ai
} // namespace camswap
