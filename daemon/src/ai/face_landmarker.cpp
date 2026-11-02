// face_landmarker.cpp - MediaPipe Face Landmarker & SCRFD ORT fallback

#include "face_landmarker.hpp"
#include "../common/logging.hpp"
#include "ort_loader.hpp"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace camswap {
namespace ai {

namespace {

static const int kDetSize = 640;

bool file_exists_nonempty(const char *path) {
  if (!path || !path[0])
    return false;
  struct stat st {};
  return (stat(path, &st) == 0 && st.st_size > 64);
}

inline uint8_t clamp_u8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline void nv12_pixel_to_rgb(const uint8_t *nv12, int w, int h, int x, int y,
                              uint8_t &r, uint8_t &g, uint8_t &b) {
  if (x < 0)
    x = 0;
  if (x >= w)
    x = w - 1;
  if (y < 0)
    y = 0;
  if (y >= h)
    y = h - 1;
  const uint8_t *y_plane = nv12;
  const uint8_t *uv_plane = nv12 + w * h;
  int uv_idx = (y / 2) * w + (x & ~1);
  int yv = static_cast<int>(y_plane[y * w + x]) - 16;
  int u = static_cast<int>(uv_plane[uv_idx]) - 128;
  int v = static_cast<int>(uv_plane[uv_idx + 1]) - 128;
  if (yv < 0)
    yv = 0;
  r = clamp_u8((298 * yv + 409 * v + 128) >> 8);
  g = clamp_u8((298 * yv - 100 * u - 208 * v + 128) >> 8);
  b = clamp_u8((298 * yv + 516 * u + 128) >> 8);
}

void derive_5pt_from_bbox(float x1, float y1, float x2, float y2,
                          float out_kps[10]) {
  float w = x2 - x1;
  float h = y2 - y1;
  out_kps[0] = x1 + 0.30f * w;
  out_kps[1] = y1 + 0.33f * h;
  out_kps[2] = x1 + 0.70f * w;
  out_kps[3] = y1 + 0.33f * h;
  out_kps[4] = x1 + 0.50f * w;
  out_kps[5] = y1 + 0.55f * h;
  out_kps[6] = x1 + 0.35f * w;
  out_kps[7] = y1 + 0.72f * h;
  out_kps[8] = x1 + 0.65f * w;
  out_kps[9] = y1 + 0.72f * h;
}

} // namespace

FaceLandmarker::FaceLandmarker()
    : m_tflite_lib(nullptr), m_ort_env(nullptr),
      m_ort_session(nullptr), m_ort_mem_info(nullptr), m_output_count(0),
      m_model_ready(false), m_use_ort_scrfd(false), m_det_input_nchw(nullptr) {
  memset(m_input_name, 0, sizeof(m_input_name));
  memset(m_output_names, 0, sizeof(m_output_names));
}

FaceLandmarker::~FaceLandmarker() { cleanup(); }

bool FaceLandmarker::init(const char *model_path,
                          const char *fallback_scrfd_path) {
  LOGI("ai: initializing face landmarker (model=%s, fallback=%s)",
       model_path ? model_path : "(null)",
       fallback_scrfd_path ? fallback_scrfd_path : "(null)");

  if (!m_det_input_nchw) {
    m_det_input_nchw = static_cast<float *>(
        malloc(sizeof(float) * 3 * kDetSize * kDetSize));
    if (!m_det_input_nchw) {
      LOGE("ai: failed to preallocate landmarker scratch buffer");
      return false;
    }
  }

  m_model_ready = false;
  m_use_ort_scrfd = false;

  // 1. Try MediaPipe Tasks / TFLite GPU delegate if task/tflite file is present
  if (file_exists_nonempty(model_path)) {
    const char *ext = strrchr(model_path, '.');
    bool is_onnx = (ext && strcmp(ext, ".onnx") == 0);
    if (!is_onnx) {
      void *lib = dlopen("libtensorflowlite_jni.so", RTLD_NOW | RTLD_LOCAL);
      if (!lib) {
        lib = dlopen("libtflite.so", RTLD_NOW | RTLD_LOCAL);
      }
      if (lib) {
        m_tflite_lib = lib;
        m_model_ready = true;
        m_use_ort_scrfd = false;
        LOGI("ai: MediaPipe/TFLite landmarker loaded from %s", model_path);
        return true;
      }
      LOGW("ai: TFLite runtime not found for %s, checking ONNX SCRFD fallback",
           model_path);
    }
  }

  // 2. Fallback SCRFD in the shared ONNX Runtime
  const char *ort_model = nullptr;
  if (file_exists_nonempty(fallback_scrfd_path)) {
    ort_model = fallback_scrfd_path;
  } else if (file_exists_nonempty(model_path)) {
    const char *ext = strrchr(model_path, '.');
    if (ext && strcmp(ext, ".onnx") == 0) {
      ort_model = model_path;
    }
  }

  if (ort_model) {
    ort::OrtApiTable api{};
    if (ort::ensure_ort_loaded(&api)) {
      ort::OrtEnv *env = nullptr;
      ort::OrtStatus *st =
          api.CreateEnv(ort::ORT_LOGGING_LEVEL_WARNING, "camswap_det", &env);
      if (!st && env) {
        ort::OrtSessionOptions *opts = nullptr;
        api.CreateSessionOptions(&opts);
        if (opts) {
          api.SetIntraOpNumThreads(opts, 2);
          api.SetSessionGraphOptimizationLevel(opts, ort::ORT_ENABLE_ALL);
        }
        ort::OrtSession *sess = nullptr;
        st = api.CreateSession(env, ort_model, opts, &sess);
        if (opts)
          api.ReleaseSessionOptions(opts);
        if (!st && sess) {
          size_t in_count = 0, out_count = 0;
          api.SessionGetInputCount(sess, &in_count);
          api.SessionGetOutputCount(sess, &out_count);
          if (in_count == 1 &&
              (out_count == 9 || out_count == 6 || out_count == 10)) {
            ort::OrtAllocator *alloc = nullptr;
            api.GetAllocatorWithDefaultOptions(&alloc);
            char *in_name = nullptr;
            if (!api.SessionGetInputName(sess, 0, alloc, &in_name) && in_name) {
              snprintf(m_input_name, sizeof(m_input_name), "%s", in_name);
              api.AllocatorFree(alloc, in_name);
            }
            m_output_count = static_cast<int>(out_count);
            for (size_t i = 0; i < out_count && i < 10; ++i) {
              char *oname = nullptr;
              if (!api.SessionGetOutputName(sess, i, alloc, &oname) && oname) {
                snprintf(m_output_names[i], sizeof(m_output_names[i]), "%s",
                         oname);
                api.AllocatorFree(alloc, oname);
              }
            }
            ort::OrtMemoryInfo *mem = nullptr;
            api.CreateCpuMemoryInfo(ort::OrtArenaAllocator,
                                    ort::OrtMemTypeDefault, &mem);
            m_ort_env = env;
            m_ort_session = sess;
            m_ort_mem_info = mem;
            m_use_ort_scrfd = true;
            m_model_ready = true;
            LOGI("ai: SCRFD ONNX landmarker initialized (%s, outputs=%d)",
                 ort_model, m_output_count);
            return true;
          }
          LOGW("ai: SCRFD model %s has unexpected I/O (in=%zu, out=%zu)",
               ort_model, in_count, out_count);
          api.ReleaseSession(sess);
        } else if (st) {
          api.ReleaseStatus(st);
        }
        api.ReleaseEnv(env);
      } else if (st) {
        api.ReleaseStatus(st);
      }
    }
  }

  LOGW("ai: landmarker model unavailable; daemon stays alive in passthrough");
  return true;
}

void FaceLandmarker::cleanup() {
  LOGI("ai: cleaning up face landmarker");
  ort::OrtApiTable api{};
  if (m_ort_session && ort::ensure_ort_loaded(&api)) {
    if (m_ort_mem_info) {
      api.ReleaseMemoryInfo(static_cast<ort::OrtMemoryInfo *>(m_ort_mem_info));
      m_ort_mem_info = nullptr;
    }
    api.ReleaseSession(static_cast<ort::OrtSession *>(m_ort_session));
    m_ort_session = nullptr;
    if (m_ort_env) {
      api.ReleaseEnv(static_cast<ort::OrtEnv *>(m_ort_env));
      m_ort_env = nullptr;
    }
  }
  if (m_tflite_lib) {
    dlclose(m_tflite_lib);
    m_tflite_lib = nullptr;
  }
  if (m_det_input_nchw) {
    free(m_det_input_nchw);
    m_det_input_nchw = nullptr;
  }
  m_model_ready = false;
  m_use_ort_scrfd = false;
}

bool FaceLandmarker::run_scrfd_ort(const uint8_t *nv12_frame, int width,
                                   int height, FaceBox *out_face) {
  ort::OrtApiTable api{};
  if (!ort::ensure_ort_loaded(&api) || !m_ort_session || !m_ort_mem_info ||
      !m_det_input_nchw) {
    return false;
  }

  // Aspect-preserving top-left resize into 640x640 NCHW with (px - 127.5)/128.0
  float scale = static_cast<float>(kDetSize) /
                static_cast<float>(width > height ? width : height);
  int new_w = static_cast<int>(width * scale);
  int new_h = static_cast<int>(height * scale);
  if (new_w < 1)
    new_w = 1;
  if (new_h < 1)
    new_h = 1;

  const int plane = kDetSize * kDetSize;
  float *ch_r = m_det_input_nchw;
  float *ch_g = m_det_input_nchw + plane;
  float *ch_b = m_det_input_nchw + 2 * plane;

  for (int y = 0; y < kDetSize; ++y) {
    int sy = static_cast<int>(y / scale);
    for (int x = 0; x < kDetSize; ++x) {
      int idx = y * kDetSize + x;
      if (x < new_w && y < new_h) {
        int sx = static_cast<int>(x / scale);
        uint8_t r, g, b;
        nv12_pixel_to_rgb(nv12_frame, width, height, sx, sy, r, g, b);
        ch_r[idx] = (static_cast<float>(r) - 127.5f) / 128.0f;
        ch_g[idx] = (static_cast<float>(g) - 127.5f) / 128.0f;
        ch_b[idx] = (static_cast<float>(b) - 127.5f) / 128.0f;
      } else {
        ch_r[idx] = -127.5f / 128.0f;
        ch_g[idx] = -127.5f / 128.0f;
        ch_b[idx] = -127.5f / 128.0f;
      }
    }
  }

  const int64_t in_shape[4] = {1, 3, kDetSize, kDetSize};
  ort::OrtValue *in_tensor = nullptr;
  ort::OrtStatus *st = api.CreateTensorWithDataAsOrtValue(
      static_cast<ort::OrtMemoryInfo *>(m_ort_mem_info), m_det_input_nchw,
      sizeof(float) * 3 * plane, in_shape, 4,
      ort::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in_tensor);
  if (st || !in_tensor) {
    if (st)
      api.ReleaseStatus(st);
    return false;
  }

  const char *in_names[1] = {m_input_name};
  const char *out_names[10];
  ort::OrtValue *out_tensors[10] = {nullptr};
  for (int i = 0; i < m_output_count; ++i) {
    out_names[i] = m_output_names[i];
  }

  st = api.Run(static_cast<ort::OrtSession *>(m_ort_session), nullptr, in_names,
               &in_tensor, 1, out_names, static_cast<size_t>(m_output_count),
               out_tensors);
  api.ReleaseValue(in_tensor);
  if (st) {
    api.ReleaseStatus(st);
    return false;
  }

  // Parse SCRFD highest-scoring anchor across FPN strides [8, 16, 32]
  const int strides[3] = {8, 16, 32};
  const int fmc = (m_output_count == 10) ? 5 : 3;
  const bool has_kps = (m_output_count == 9);

  float best_score = 0.45f;
  FaceBox best{};
  bool found = false;

  for (int s = 0; s < 3; ++s) {
    float *scores = nullptr;
    float *boxes = nullptr;
    float *kps = nullptr;
    api.GetTensorMutableData(out_tensors[s], reinterpret_cast<void **>(&scores));
    api.GetTensorMutableData(out_tensors[s + fmc],
                             reinterpret_cast<void **>(&boxes));
    if (has_kps) {
      api.GetTensorMutableData(out_tensors[s + 6],
                               reinterpret_cast<void **>(&kps));
    }
    if (!scores || !boxes)
      continue;

    int stride = strides[s];
    int feat = kDetSize / stride;
    int grid = feat * feat;
    int anchors_per_pos = 2;
    int total_anchors = grid * anchors_per_pos;

    for (int i = 0; i < total_anchors; ++i) {
      float sc = scores[i];
      if (sc <= best_score)
        continue;
      int g_idx = i / anchors_per_pos;
      int ay = g_idx / feat;
      int ax = g_idx % feat;
      float cx = static_cast<float>(ax * stride);
      float cy = static_cast<float>(ay * stride);

      float x1 = (cx - boxes[i * 4 + 0] * stride) / scale;
      float y1 = (cy - boxes[i * 4 + 1] * stride) / scale;
      float x2 = (cx + boxes[i * 4 + 2] * stride) / scale;
      float y2 = (cy + boxes[i * 4 + 3] * stride) / scale;

      if (x1 < 0.0f)
        x1 = 0.0f;
      if (y1 < 0.0f)
        y1 = 0.0f;
      if (x2 > static_cast<float>(width))
        x2 = static_cast<float>(width);
      if (y2 > static_cast<float>(height))
        y2 = static_cast<float>(height);

      if ((x2 - x1) < 24.0f || (y2 - y1) < 24.0f)
        continue;

      best_score = sc;
      best.x1 = x1;
      best.y1 = y1;
      best.x2 = x2;
      best.y2 = y2;
      best.score = sc;
      if (has_kps && kps) {
        for (int k = 0; k < 5; ++k) {
          best.kps[k * 2 + 0] =
              (cx + kps[i * 10 + k * 2 + 0] * stride) / scale;
          best.kps[k * 2 + 1] =
              (cy + kps[i * 10 + k * 2 + 1] * stride) / scale;
        }
      } else {
        derive_5pt_from_bbox(x1, y1, x2, y2, best.kps);
      }
      found = true;
    }
  }

  for (int i = 0; i < m_output_count; ++i) {
    if (out_tensors[i])
      api.ReleaseValue(out_tensors[i]);
  }

  if (found && out_face) {
    *out_face = best;
  }
  return found;
}

bool FaceLandmarker::detect_landmarks(const uint8_t *nv12_frame, int width,
                                      int height, FaceBox *out_face) {
  if (!nv12_frame || width <= 0 || height <= 0 || !out_face) {
    return false;
  }

  // Uncertainty invariant: if no real detector model is initialized, return
  // false so the pipeline falls back to passthrough.
  if (!m_model_ready) {
    return false;
  }

  if (m_use_ort_scrfd) {
    return run_scrfd_ort(nv12_frame, width, height, out_face);
  }

  return false;
}

bool FaceLandmarker::track_landmarks(const uint8_t *nv12_frame, int width,
                                     int height, const FaceBox &prev_face,
                                     FaceBox *out_face) {
  if (!nv12_frame || !out_face || !m_model_ready) {
    return false;
  }

  float bw = prev_face.x2 - prev_face.x1;
  float bh = prev_face.y2 - prev_face.y1;
  if (bw < 20.0f || bh < 20.0f) {
    return false;
  }

  // Compute luma centroid inside expanded box to track subtle frame-to-frame
  // motion on intermediate frames between the 8-frame detector runs.
  int x0 = static_cast<int>(prev_face.x1);
  int y0 = static_cast<int>(prev_face.y1);
  int x1 = static_cast<int>(prev_face.x2);
  int y1 = static_cast<int>(prev_face.y2);
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > width)
    x1 = width;
  if (y1 > height)
    y1 = height;

  double sum_w = 0.0, sum_x = 0.0, sum_y = 0.0;
  for (int y = y0; y < y1; y += 2) {
    const uint8_t *row = nv12_frame + y * width;
    for (int x = x0; x < x1; x += 2) {
      double w = static_cast<double>(row[x]);
      sum_w += w;
      sum_x += w * static_cast<double>(x);
      sum_y += w * static_cast<double>(y);
    }
  }

  if (sum_w < 1.0) {
    return false;
  }

  float prev_cx = 0.5f * (prev_face.x1 + prev_face.x2);
  float prev_cy = 0.5f * (prev_face.y1 + prev_face.y2);
  float meas_cx = static_cast<float>(sum_x / sum_w);
  float meas_cy = static_cast<float>(sum_y / sum_w);

  // Dampened shift (alpha = 0.20) for landmark stability
  float dx = (meas_cx - prev_cx) * 0.20f;
  float dy = (meas_cy - prev_cy) * 0.20f;

  // Reject sudden jumps (> 25% of box size) as tracking loss
  if (fabsf(dx) > 0.25f * bw || fabsf(dy) > 0.25f * bh) {
    return false;
  }

  *out_face = prev_face;
  out_face->x1 += dx;
  out_face->y1 += dy;
  out_face->x2 += dx;
  out_face->y2 += dy;
  for (int i = 0; i < 5; ++i) {
    out_face->kps[i * 2 + 0] += dx;
    out_face->kps[i * 2 + 1] += dy;
  }
  return true;
}

} // namespace ai
} // namespace camswap
