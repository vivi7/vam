// face_swap.cpp - INSwapper 512 int8 face swap and Umeyama alignment

#include "face_swap.hpp"
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

namespace ort {

namespace {

// Minimal layout of OrtApiBase and OrtApi (ONNX Runtime C API v11..v18)
struct RawOrtApiBase {
  const void *(*GetApi)(uint32_t version);
  const char *(*GetVersionString)(void);
};

// Raw OrtApi function pointer array indexed by official OrtApi struct order
// in onnxruntime_c_api.h:
// 0: CreateStatus
// 1: GetErrorCode
// 2: GetErrorMessage
// 3: CreateEnv
// 4: CreateEnvWithCustomLogger
// 5: EnableTelemetryEvents
// 6: DisableTelemetryEvents
// 7: CreateSession
// 8: CreateSessionFromArray
// 9: Run
// 10: CreateSessionOptions
// 11: SetOptimizedModelFilePath
// 12: CloneSessionOptions
// 13: SetSessionExecutionMode
// 14: EnableProfiling
// 15: DisableProfiling
// 16: EnableMemPattern
// 17: DisableMemPattern
// 18: EnableCpuMemArena
// 19: DisableCpuMemArena
// 20: SetSessionLogId
// 21: SetSessionLogVerbosityLevel
// 22: SetSessionLogSeverityLevel
// 23: SetSessionGraphOptimizationLevel
// 24: SetIntraOpNumThreads
// 25: SetInterOpNumThreads
// ...
// 36: SessionGetInputCount
// 37: SessionGetOutputCount
// 38: SessionGetOverridableInitializerCount
// 39: SessionGetInputTypeInfo
// 40: SessionGetOutputTypeInfo
// 41: SessionGetOverridableInitializerTypeInfo
// 42: SessionGetInputName
// 43: SessionGetOutputName
// ...
// 47: CreateTensorWithDataAsOrtValue
// 48: IsTensor
// 49: GetTensorMutableData
// ...
// 55: CastTypeInfoToTensorInfo
// ...
// 61: GetDimensionsCount
// 62: GetDimensions
// ...
// 64: CreateCpuMemoryInfo
// ...
// 66: GetTensorTypeAndShape
// ...
// 78: GetAllocatorWithDefaultOptions
// 79: AllocatorFree
// ...
// 92: ReleaseEnv
// 93: ReleaseStatus
// 94: ReleaseMemoryInfo
// 95: ReleaseSession
// 96: ReleaseValue
// 97: ReleaseRunOptions
// 98: ReleaseTypeInfo
// 99: ReleaseTensorTypeAndShapeInfo
// 100: ReleaseSessionOptions

static void *g_ort_handle = nullptr;
static OrtApiTable g_table{};
static bool g_ort_checked = false;
static bool g_ort_ok = false;

} // namespace

bool ensure_ort_loaded(OrtApiTable *out_table) {
  if (g_ort_checked) {
    if (g_ort_ok && out_table) {
      *out_table = g_table;
    }
    return g_ort_ok;
  }
  g_ort_checked = true;

  const char *candidates[] = {
      "/data/adb/modules/camswap/system/lib64/libonnxruntime.so",
      "/system/lib64/libonnxruntime.so",
      "libonnxruntime.so"};

  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    g_ort_handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
    if (g_ort_handle) {
      LOGI("ai: loaded %s", candidates[i]);
      break;
    }
  }

  if (!g_ort_handle) {
    LOGW("ai: libonnxruntime.so not found; AI sessions will run in passthrough");
    return false;
  }

  using GetApiBaseFn = const RawOrtApiBase *(*)(void);
  GetApiBaseFn get_base = reinterpret_cast<GetApiBaseFn>(
      dlsym(g_ort_handle, "OrtGetApiBase"));
  if (!get_base) {
    LOGW("ai: OrtGetApiBase symbol missing in libonnxruntime.so");
    return false;
  }

  const RawOrtApiBase *base = get_base();
  if (!base || !base->GetApi) {
    return false;
  }

  const void *raw = nullptr;
  for (uint32_t ver = 17; ver >= 11; --ver) {
    raw = base->GetApi(ver);
    if (raw)
      break;
  }
  if (!raw) {
    return false;
  }

  void *const *fn = reinterpret_cast<void *const *>(const_cast<void *>(raw));
  g_table.raw_api = raw;
  g_table.GetErrorMessage =
      reinterpret_cast<decltype(g_table.GetErrorMessage)>(fn[2]);
  g_table.CreateEnv = reinterpret_cast<decltype(g_table.CreateEnv)>(fn[3]);
  g_table.CreateSession =
      reinterpret_cast<decltype(g_table.CreateSession)>(fn[7]);
  g_table.Run = reinterpret_cast<decltype(g_table.Run)>(fn[9]);
  g_table.CreateSessionOptions =
      reinterpret_cast<decltype(g_table.CreateSessionOptions)>(fn[10]);
  g_table.SetSessionGraphOptimizationLevel =
      reinterpret_cast<decltype(g_table.SetSessionGraphOptimizationLevel)>(
          fn[23]);
  g_table.SetIntraOpNumThreads =
      reinterpret_cast<decltype(g_table.SetIntraOpNumThreads)>(fn[24]);
  g_table.SessionGetInputCount =
      reinterpret_cast<decltype(g_table.SessionGetInputCount)>(fn[36]);
  g_table.SessionGetOutputCount =
      reinterpret_cast<decltype(g_table.SessionGetOutputCount)>(fn[37]);
  g_table.SessionGetInputTypeInfo =
      reinterpret_cast<decltype(g_table.SessionGetInputTypeInfo)>(fn[39]);
  g_table.SessionGetOutputTypeInfo =
      reinterpret_cast<decltype(g_table.SessionGetOutputTypeInfo)>(fn[40]);
  g_table.SessionGetInputName =
      reinterpret_cast<decltype(g_table.SessionGetInputName)>(fn[42]);
  g_table.SessionGetOutputName =
      reinterpret_cast<decltype(g_table.SessionGetOutputName)>(fn[43]);
  g_table.CreateTensorWithDataAsOrtValue =
      reinterpret_cast<decltype(g_table.CreateTensorWithDataAsOrtValue)>(
          fn[47]);
  g_table.GetTensorMutableData =
      reinterpret_cast<decltype(g_table.GetTensorMutableData)>(fn[49]);
  g_table.CastTypeInfoToTensorInfo =
      reinterpret_cast<decltype(g_table.CastTypeInfoToTensorInfo)>(fn[55]);
  g_table.GetDimensionsCount =
      reinterpret_cast<decltype(g_table.GetDimensionsCount)>(fn[61]);
  g_table.GetDimensions =
      reinterpret_cast<decltype(g_table.GetDimensions)>(fn[62]);
  g_table.CreateCpuMemoryInfo =
      reinterpret_cast<decltype(g_table.CreateCpuMemoryInfo)>(fn[64]);
  g_table.GetTensorTypeAndShape =
      reinterpret_cast<decltype(g_table.GetTensorTypeAndShape)>(fn[66]);
  g_table.GetAllocatorWithDefaultOptions =
      reinterpret_cast<decltype(g_table.GetAllocatorWithDefaultOptions)>(
          fn[78]);
  g_table.AllocatorFree =
      reinterpret_cast<decltype(g_table.AllocatorFree)>(fn[79]);
  g_table.ReleaseEnv = reinterpret_cast<decltype(g_table.ReleaseEnv)>(fn[92]);
  g_table.ReleaseStatus =
      reinterpret_cast<decltype(g_table.ReleaseStatus)>(fn[93]);
  g_table.ReleaseMemoryInfo =
      reinterpret_cast<decltype(g_table.ReleaseMemoryInfo)>(fn[94]);
  g_table.ReleaseSession =
      reinterpret_cast<decltype(g_table.ReleaseSession)>(fn[95]);
  g_table.ReleaseValue =
      reinterpret_cast<decltype(g_table.ReleaseValue)>(fn[96]);
  g_table.ReleaseTypeInfo =
      reinterpret_cast<decltype(g_table.ReleaseTypeInfo)>(fn[98]);
  g_table.ReleaseTensorTypeAndShapeInfo =
      reinterpret_cast<decltype(g_table.ReleaseTensorTypeAndShapeInfo)>(fn[99]);
  g_table.ReleaseSessionOptions =
      reinterpret_cast<decltype(g_table.ReleaseSessionOptions)>(fn[100]);

  g_ort_ok = true;
  if (out_table) {
    *out_table = g_table;
  }
  return true;
}

} // namespace ort

namespace {

static const uint32_t kMaxSwapCrop = 512;

// InsightFace / FFHQ canonical 128x128 reference points (from
// refs/android-face-fusion/ARCHITECTURE.md):
// ArcFace 112x112 points shifted by +8.0 in X for 128x128, then scaled by
// (crop_size / 128.0) for 512x512 (`inswapper_512_live`).
static const float kRef128[5][2] = {
    {38.2946f + 8.0f, 51.6963f}, // left eye
    {73.5318f + 8.0f, 51.5014f}, // right eye
    {56.0252f + 8.0f, 71.7366f}, // nose
    {41.5493f + 8.0f, 92.3655f}, // left mouth
    {70.7299f + 8.0f, 92.2041f}  // right mouth
};

bool file_exists_nonempty(const char *path) {
  if (!path || !path[0])
    return false;
  struct stat st {};
  return (stat(path, &st) == 0 && st.st_size > 256);
}

inline uint8_t clamp_u8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline void nv12_sample_rgb(const uint8_t *nv12, uint32_t w, uint32_t h, int x,
                            int y, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (x < 0)
    x = 0;
  if (x >= static_cast<int>(w))
    x = static_cast<int>(w) - 1;
  if (y < 0)
    y = 0;
  if (y >= static_cast<int>(h))
    y = static_cast<int>(h) - 1;
  const uint8_t *y_plane = nv12;
  const uint8_t *uv_plane = nv12 + w * h;
  uint32_t uv_idx =
      static_cast<uint32_t>(y / 2) * w + static_cast<uint32_t>(x & ~1);
  int yv = static_cast<int>(y_plane[static_cast<uint32_t>(y) * w +
                                    static_cast<uint32_t>(x)]) -
           16;
  int u = static_cast<int>(uv_plane[uv_idx]) - 128;
  int v = static_cast<int>(uv_plane[uv_idx + 1]) - 128;
  if (yv < 0)
    yv = 0;
  r = clamp_u8((298 * yv + 409 * v + 128) >> 8);
  g = clamp_u8((298 * yv - 100 * u - 208 * v + 128) >> 8);
  b = clamp_u8((298 * yv + 516 * u + 128) >> 8);
}

} // namespace

FaceSwap::FaceSwap()
    : m_ort_env(nullptr), m_ort_session(nullptr), m_ort_mem_info(nullptr),
      m_model_crop_size(512), m_model_ready(false), m_input_nchw(nullptr),
      m_output_nchw(nullptr), m_mask_scratch_a(nullptr),
      m_mask_scratch_b(nullptr), m_cached_mask_size(0) {
  memset(m_img_input_name, 0, sizeof(m_img_input_name));
  memset(m_emb_input_name, 0, sizeof(m_emb_input_name));
  memset(m_output_name, 0, sizeof(m_output_name));
}

FaceSwap::~FaceSwap() { cleanup(); }

bool FaceSwap::init(const char *model_path) {
  LOGI("ai: initializing face swap (model=%s)",
       model_path ? model_path : "(null)");

  const size_t max_pixels = static_cast<size_t>(kMaxSwapCrop) * kMaxSwapCrop;
  if (!m_input_nchw) {
    m_input_nchw = static_cast<float *>(malloc(sizeof(float) * 3 * max_pixels));
  }
  if (!m_output_nchw) {
    m_output_nchw =
        static_cast<float *>(malloc(sizeof(float) * 3 * max_pixels));
  }
  if (!m_mask_scratch_a) {
    m_mask_scratch_a = static_cast<float *>(malloc(sizeof(float) * max_pixels));
  }
  if (!m_mask_scratch_b) {
    m_mask_scratch_b = static_cast<float *>(malloc(sizeof(float) * max_pixels));
  }
  if (!m_input_nchw || !m_output_nchw || !m_mask_scratch_a ||
      !m_mask_scratch_b) {
    LOGE("ai: failed to preallocate FaceSwap scratch buffers");
    return false;
  }

  m_model_ready = false;
  m_cached_mask_size = 0;

  if (!file_exists_nonempty(model_path)) {
    LOGW("ai: swap model file missing (%s); staying in passthrough",
         model_path ? model_path : "(null)");
    return true;
  }

  ort::OrtApiTable api{};
  if (!ort::ensure_ort_loaded(&api)) {
    return true;
  }

  ort::OrtEnv *env = nullptr;
  ort::OrtStatus *st =
      api.CreateEnv(ort::ORT_LOGGING_LEVEL_WARNING, "camswap_swap", &env);
  if (st || !env) {
    if (st)
      api.ReleaseStatus(st);
    return true;
  }

  ort::OrtSessionOptions *opts = nullptr;
  api.CreateSessionOptions(&opts);
  if (opts) {
    api.SetIntraOpNumThreads(opts, 4);
    api.SetSessionGraphOptimizationLevel(opts, ort::ORT_ENABLE_ALL);
  }

  ort::OrtSession *sess = nullptr;
  st = api.CreateSession(env, model_path, opts, &sess);
  if (opts)
    api.ReleaseSessionOptions(opts);
  if (st || !sess) {
    if (st)
      api.ReleaseStatus(st);
    api.ReleaseEnv(env);
    LOGW("ai: failed to create ORT session for %s; staying in passthrough",
         model_path);
    return true;
  }

  // Verify ONNX I/O contract:
  // Exactly 2 inputs:
  //   - 4D image tensor [1, 3, H, W] (H == W == 512 or 128)
  //   - 2D embedding tensor [1, 512]
  // And 1 output [1, 3, H, W].
  size_t in_count = 0, out_count = 0;
  api.SessionGetInputCount(sess, &in_count);
  api.SessionGetOutputCount(sess, &out_count);

  bool io_valid = false;
  if (in_count == 2 && out_count >= 1) {
    ort::OrtAllocator *alloc = nullptr;
    api.GetAllocatorWithDefaultOptions(&alloc);

    bool found_img = false;
    bool found_emb = false;

    for (size_t i = 0; i < 2; ++i) {
      char *name = nullptr;
      if (api.SessionGetInputName(sess, i, alloc, &name) || !name)
        continue;

      ort::OrtTypeInfo *tinfo = nullptr;
      if (!api.SessionGetInputTypeInfo(sess, i, &tinfo) && tinfo) {
        const ort::OrtTensorTypeAndShapeInfo *sinfo = nullptr;
        if (!api.CastTypeInfoToTensorInfo(tinfo, &sinfo) && sinfo) {
          size_t dim_count = 0;
          api.GetDimensionsCount(sinfo, &dim_count);
          int64_t dims[8] = {0};
          if (dim_count <= 8) {
            api.GetDimensions(sinfo, dims, dim_count);
          }
          if (dim_count == 4 && (dims[1] == 3 || dims[1] == -1)) {
            snprintf(m_img_input_name, sizeof(m_img_input_name), "%s", name);
            if (dims[2] == 128 || dims[2] == 512) {
              m_model_crop_size = static_cast<uint32_t>(dims[2]);
            } else {
              m_model_crop_size = 512;
            }
            found_img = true;
          } else if (dim_count == 2 && (dims[1] == 512 || dims[1] == -1)) {
            snprintf(m_emb_input_name, sizeof(m_emb_input_name), "%s", name);
            found_emb = true;
          }
        }
        api.ReleaseTypeInfo(tinfo);
      }
      api.AllocatorFree(alloc, name);
    }

    char *out_name = nullptr;
    if (!api.SessionGetOutputName(sess, 0, alloc, &out_name) && out_name) {
      snprintf(m_output_name, sizeof(m_output_name), "%s", out_name);
      api.AllocatorFree(alloc, out_name);
    }

    io_valid = (found_img && found_emb && m_output_name[0] != '\0');
  }

  if (!io_valid) {
    LOGW("ai: ONNX model %s I/O mismatch (expected image[1,3,H,W] + "
         "emb[1,512]); forcing passthrough",
         model_path);
    api.ReleaseSession(sess);
    api.ReleaseEnv(env);
    return true;
  }

  ort::OrtMemoryInfo *mem = nullptr;
  api.CreateCpuMemoryInfo(ort::OrtArenaAllocator, ort::OrtMemTypeDefault, &mem);

  m_ort_env = env;
  m_ort_session = sess;
  m_ort_mem_info = mem;
  m_model_ready = true;
  LOGI("ai: INSwapper ORT session ready (crop=%u, img=%s, emb=%s, out=%s)",
       m_model_crop_size, m_img_input_name, m_emb_input_name, m_output_name);
  return true;
}

void FaceSwap::cleanup() {
  LOGI("ai: cleaning up face swap");
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
  if (m_input_nchw) {
    free(m_input_nchw);
    m_input_nchw = nullptr;
  }
  if (m_output_nchw) {
    free(m_output_nchw);
    m_output_nchw = nullptr;
  }
  if (m_mask_scratch_a) {
    free(m_mask_scratch_a);
    m_mask_scratch_a = nullptr;
  }
  if (m_mask_scratch_b) {
    free(m_mask_scratch_b);
    m_mask_scratch_b = nullptr;
  }
  m_model_ready = false;
  m_cached_mask_size = 0;
}

bool FaceSwap::estimate_umeyama_5pt(const float src_kps[10], uint32_t crop_size,
                                    float out_fwd_2x3[6],
                                    float out_inv_2x3[6]) {
  if (!src_kps || !out_fwd_2x3 || !out_inv_2x3 || crop_size == 0) {
    return false;
  }

  const float scale_ref = static_cast<float>(crop_size) / 128.0f;
  float dst[5][2];
  float src[5][2];

  float src_cx = 0.0f, src_cy = 0.0f;
  float dst_cx = 0.0f, dst_cy = 0.0f;

  for (int i = 0; i < 5; ++i) {
    src[i][0] = src_kps[i * 2 + 0];
    src[i][1] = src_kps[i * 2 + 1];
    dst[i][0] = kRef128[i][0] * scale_ref;
    dst[i][1] = kRef128[i][1] * scale_ref;

    src_cx += src[i][0];
    src_cy += src[i][1];
    dst_cx += dst[i][0];
    dst_cy += dst[i][1];
  }

  src_cx *= 0.2f;
  src_cy *= 0.2f;
  dst_cx *= 0.2f;
  dst_cy *= 0.2f;

  float src_norm = 0.0f;
  float a = 0.0f;
  float b = 0.0f;

  for (int i = 0; i < 5; ++i) {
    float sx = src[i][0] - src_cx;
    float sy = src[i][1] - src_cy;
    float dx = dst[i][0] - dst_cx;
    float dy = dst[i][1] - dst_cy;

    src_norm += sx * sx + sy * sy;
    a += sx * dx + sy * dy;
    b += sx * dy - sy * dx;
  }

  if (src_norm < 1e-6f) {
    return false;
  }

  // Closed-form 2D Procrustes similarity transform (matches
  // refs/android-face-fusion/ARCHITECTURE.md):
  //   M_fwd = [[a/src_norm, -b/src_norm, tx],
  //            [b/src_norm,  a/src_norm, ty]]
  float a_mat = a / src_norm;
  float b_mat = -b / src_norm;
  float d_mat = b / src_norm;
  float e_mat = a / src_norm;

  float c_mat = dst_cx - (a_mat * src_cx + b_mat * src_cy);
  float f_mat = dst_cy - (d_mat * src_cx + e_mat * src_cy);

  float det = a_mat * e_mat - b_mat * d_mat;
  if (fabsf(det) < 1e-8f || isnan(det) || isinf(det)) {
    return false;
  }

  out_fwd_2x3[0] = a_mat;
  out_fwd_2x3[1] = b_mat;
  out_fwd_2x3[2] = c_mat;
  out_fwd_2x3[3] = d_mat;
  out_fwd_2x3[4] = e_mat;
  out_fwd_2x3[5] = f_mat;

  float inv_det = 1.0f / det;
  float ia = e_mat * inv_det;
  float ib = -b_mat * inv_det;
  float id = -d_mat * inv_det;
  float ie = a_mat * inv_det;
  float ic = -(ia * c_mat + ib * f_mat);
  float if_ = -(id * c_mat + ie * f_mat);

  out_inv_2x3[0] = ia;
  out_inv_2x3[1] = ib;
  out_inv_2x3[2] = ic;
  out_inv_2x3[3] = id;
  out_inv_2x3[4] = ie;
  out_inv_2x3[5] = if_;
  return true;
}

void FaceSwap::build_soft_mask(uint32_t crop_size, uint8_t *out_mask_u8) {
  // Build eroded + Gaussian-blurred blending mask in crop coordinates
  // matching refs/android-face-fusion/ARCHITECTURE.md:
  //   k_erode = max(crop_size / 10, 10)
  //   k_blur  = 2 * max(crop_size / 20, 5) + 1
  const int sz = static_cast<int>(crop_size);
  const int erode_k = (sz / 10 > 10) ? (sz / 10) : 10;
  const int blur_half = (sz / 20 > 5) ? (sz / 20) : 5;
  const int blur_k = 2 * blur_half + 1;
  const float sigma = 0.3f * ((blur_k - 1) * 0.5f - 1.0f) + 0.8f;

  const int margin = erode_k / 2 + 4;
  for (int y = 0; y < sz; ++y) {
    for (int x = 0; x < sz; ++x) {
      bool inside =
          (x >= margin && x < (sz - margin) && y >= margin && y < (sz - margin));
      m_mask_scratch_a[y * sz + x] = inside ? 255.0f : 0.0f;
    }
  }

  // 1D Gaussian kernel
  float kernel[65];
  int hk = blur_half;
  if (hk > 32)
    hk = 32;
  float ksum = 0.0f;
  for (int i = -hk; i <= hk; ++i) {
    float w = expf(-static_cast<float>(i * i) / (2.0f * sigma * sigma));
    kernel[i + hk] = w;
    ksum += w;
  }
  for (int i = 0; i <= 2 * hk; ++i) {
    kernel[i] /= ksum;
  }

  // Horizontal pass
  for (int y = 0; y < sz; ++y) {
    for (int x = 0; x < sz; ++x) {
      float acc = 0.0f;
      for (int k = -hk; k <= hk; ++k) {
        int nx = x + k;
        float v = (nx >= 0 && nx < sz) ? m_mask_scratch_a[y * sz + nx] : 0.0f;
        acc += v * kernel[k + hk];
      }
      m_mask_scratch_b[y * sz + x] = acc;
    }
  }

  // Vertical pass and output uint8 mask
  for (int y = 0; y < sz; ++y) {
    for (int x = 0; x < sz; ++x) {
      float acc = 0.0f;
      for (int k = -hk; k <= hk; ++k) {
        int ny = y + k;
        float v = (ny >= 0 && ny < sz) ? m_mask_scratch_b[ny * sz + x] : 0.0f;
        acc += v * kernel[k + hk];
      }
      int iv = static_cast<int>(acc + 0.5f);
      out_mask_u8[y * sz + x] =
          static_cast<uint8_t>(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
    }
  }
}

bool FaceSwap::swap_face(const uint8_t *nv12_frame, uint32_t frame_w,
                         uint32_t frame_h, const float kps_5pt[10],
                         const float latent_512[512], uint32_t crop_size,
                         uint8_t *out_target_crop_rgb,
                         uint8_t *out_swapped_crop_rgb, uint8_t *out_mask_u8,
                         float out_fwd_affine_2x3[6], uint32_t *out_roi_x0,
                         uint32_t *out_roi_y0, uint32_t *out_roi_w,
                         uint32_t *out_roi_h) {
  if (!m_model_ready || !nv12_frame || !kps_5pt || !latent_512 ||
      !out_target_crop_rgb || !out_swapped_crop_rgb || !out_mask_u8 ||
      !out_fwd_affine_2x3) {
    return false;
  }

  if (crop_size != 128 && crop_size != 512) {
    crop_size = 512;
  }

  float inv_affine[6] = {0};
  if (!estimate_umeyama_5pt(kps_5pt, crop_size, out_fwd_affine_2x3,
                            inv_affine)) {
    return false;
  }

  // Compute bounding ROI in frame coordinates from the 4 corners of crop_size
  const float corners[4][2] = {{0.0f, 0.0f},
                               {static_cast<float>(crop_size), 0.0f},
                               {0.0f, static_cast<float>(crop_size)},
                               {static_cast<float>(crop_size),
                                static_cast<float>(crop_size)}};
  float min_x = static_cast<float>(frame_w);
  float min_y = static_cast<float>(frame_h);
  float max_x = 0.0f;
  float max_y = 0.0f;
  for (int i = 0; i < 4; ++i) {
    float fx = inv_affine[0] * corners[i][0] + inv_affine[1] * corners[i][1] +
               inv_affine[2];
    float fy = inv_affine[3] * corners[i][0] + inv_affine[4] * corners[i][1] +
               inv_affine[5];
    if (fx < min_x)
      min_x = fx;
    if (fy < min_y)
      min_y = fy;
    if (fx > max_x)
      max_x = fx;
    if (fy > max_y)
      max_y = fy;
  }

  int rx0 = static_cast<int>(floorf(min_x)) & ~1;
  int ry0 = static_cast<int>(floorf(min_y)) & ~1;
  int rx1 = (static_cast<int>(ceilf(max_x)) + 1) & ~1;
  int ry1 = (static_cast<int>(ceilf(max_y)) + 1) & ~1;
  if (rx0 < 0)
    rx0 = 0;
  if (ry0 < 0)
    ry0 = 0;
  if (rx1 > static_cast<int>(frame_w))
    rx1 = static_cast<int>(frame_w);
  if (ry1 > static_cast<int>(frame_h))
    ry1 = static_cast<int>(frame_h);
  if (rx1 - rx0 < 16 || ry1 - ry0 < 16) {
    return false;
  }

  if (out_roi_x0)
    *out_roi_x0 = static_cast<uint32_t>(rx0);
  if (out_roi_y0)
    *out_roi_y0 = static_cast<uint32_t>(ry0);
  if (out_roi_w)
    *out_roi_w = static_cast<uint32_t>(rx1 - rx0);
  if (out_roi_h)
    *out_roi_h = static_cast<uint32_t>(ry1 - ry0);

  // Extract aligned target face crop (crop_size x crop_size) from NV12
  for (uint32_t cy = 0; cy < crop_size; ++cy) {
    for (uint32_t cx = 0; cx < crop_size; ++cx) {
      float fx = inv_affine[0] * static_cast<float>(cx) +
                 inv_affine[1] * static_cast<float>(cy) + inv_affine[2];
      float fy = inv_affine[3] * static_cast<float>(cx) +
                 inv_affine[4] * static_cast<float>(cy) + inv_affine[5];
      uint8_t r, g, b;
      nv12_sample_rgb(nv12_frame, frame_w, frame_h,
                      static_cast<int>(fx + 0.5f), static_cast<int>(fy + 0.5f),
                      r, g, b);
      uint8_t *dst = &out_target_crop_rgb[(cy * crop_size + cx) * 3];
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
    }
  }

  // Prepare NCHW float32 input normalized by px / 255.0 (per ARCHITECTURE.md)
  const uint32_t model_sz = m_model_crop_size;
  const size_t plane = static_cast<size_t>(model_sz) * model_sz;
  float *in_r = m_input_nchw;
  float *in_g = m_input_nchw + plane;
  float *in_b = m_input_nchw + 2 * plane;

  for (uint32_t y = 0; y < model_sz; ++y) {
    uint32_t sy = (y * crop_size) / model_sz;
    for (uint32_t x = 0; x < model_sz; ++x) {
      uint32_t sx = (x * crop_size) / model_sz;
      const uint8_t *px = &out_target_crop_rgb[(sy * crop_size + sx) * 3];
      size_t idx = static_cast<size_t>(y) * model_sz + x;
      in_r[idx] = static_cast<float>(px[0]) * (1.0f / 255.0f);
      in_g[idx] = static_cast<float>(px[1]) * (1.0f / 255.0f);
      in_b[idx] = static_cast<float>(px[2]) * (1.0f / 255.0f);
    }
  }

  ort::OrtApiTable api{};
  if (!ort::ensure_ort_loaded(&api) || !m_ort_session || !m_ort_mem_info) {
    return false;
  }

  const int64_t img_shape[4] = {1, 3, static_cast<int64_t>(model_sz),
                                static_cast<int64_t>(model_sz)};
  const int64_t emb_shape[2] = {1, 512};

  ort::OrtValue *img_val = nullptr;
  ort::OrtValue *emb_val = nullptr;

  ort::OrtStatus *st = api.CreateTensorWithDataAsOrtValue(
      static_cast<ort::OrtMemoryInfo *>(m_ort_mem_info), m_input_nchw,
      sizeof(float) * 3 * plane, img_shape, 4,
      ort::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &img_val);
  if (st || !img_val) {
    if (st)
      api.ReleaseStatus(st);
    return false;
  }

  st = api.CreateTensorWithDataAsOrtValue(
      static_cast<ort::OrtMemoryInfo *>(m_ort_mem_info),
      const_cast<float *>(latent_512), sizeof(float) * 512, emb_shape, 2,
      ort::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &emb_val);
  if (st || !emb_val) {
    if (st)
      api.ReleaseStatus(st);
    api.ReleaseValue(img_val);
    return false;
  }

  const char *in_names[2] = {m_img_input_name, m_emb_input_name};
  const ort::OrtValue *in_vals[2] = {img_val, emb_val};
  const char *out_names[1] = {m_output_name};
  ort::OrtValue *out_val = nullptr;

  st = api.Run(static_cast<ort::OrtSession *>(m_ort_session), nullptr, in_names,
               in_vals, 2, out_names, 1, &out_val);
  api.ReleaseValue(img_val);
  api.ReleaseValue(emb_val);

  if (st || !out_val) {
    if (st)
      api.ReleaseStatus(st);
    return false;
  }

  float *out_data = nullptr;
  st = api.GetTensorMutableData(out_val, reinterpret_cast<void **>(&out_data));
  if (st || !out_data) {
    if (st)
      api.ReleaseStatus(st);
    api.ReleaseValue(out_val);
    return false;
  }

  // Denormalize NCHW [0,1] -> RGB uint8 crop_size x crop_size
  const float *out_r = out_data;
  const float *out_g = out_data + plane;
  const float *out_b = out_data + 2 * plane;

  for (uint32_t y = 0; y < crop_size; ++y) {
    uint32_t my = (y * model_sz) / crop_size;
    for (uint32_t x = 0; x < crop_size; ++x) {
      uint32_t mx = (x * model_sz) / crop_size;
      size_t midx = static_cast<size_t>(my) * model_sz + mx;
      uint8_t *dst = &out_swapped_crop_rgb[(y * crop_size + x) * 3];
      dst[0] = clamp_u8(static_cast<int>(out_r[midx] * 255.0f + 0.5f));
      dst[1] = clamp_u8(static_cast<int>(out_g[midx] * 255.0f + 0.5f));
      dst[2] = clamp_u8(static_cast<int>(out_b[midx] * 255.0f + 0.5f));
    }
  }

  api.ReleaseValue(out_val);

  build_soft_mask(crop_size, out_mask_u8);
  return true;
}

} // namespace ai
} // namespace camswap
