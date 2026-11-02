// face_swap.hpp - INSwapper 512 int8 face swap for camswap daemon

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace camswap {
namespace ai {

class FaceSwap {
public:
  FaceSwap();
  ~FaceSwap();

  // Initializes ONNX Runtime session for inswapper_512_live_int8.onnx.
  // Verifies I/O contract: 1 image input [1,3,H,W] + 1 embedding input [1,512]
  // and 1 image output [1,3,H,W]. If missing or mismatched, stays alive with
  // model_ready() == false so the pipeline runs in passthrough.
  bool init(const char *model_path = nullptr);
  void cleanup();

  // Warps aligned face crop (crop_size: 512 or 128 on thermal step) from NV12
  // frame using the 5-point Umeyama similarity transform, runs INSwapper
  // inference with the pre-transformed 512-float latent vector, and writes the
  // swapped RGB crop + eroded/blurred blending mask.
  bool swap_face(const uint8_t *nv12_frame, uint32_t frame_w, uint32_t frame_h,
                 const float kps_5pt[10], const float latent_512[512],
                 uint32_t crop_size, uint8_t *out_target_crop_rgb,
                 uint8_t *out_swapped_crop_rgb, uint8_t *out_mask_u8,
                 float out_fwd_affine_2x3[6], uint32_t *out_roi_x0,
                 uint32_t *out_roi_y0, uint32_t *out_roi_w,
                 uint32_t *out_roi_h);

  // Computes 2x3 Umeyama similarity transform from 5 source points to InsightFace
  // canonical template scaled to crop_size (512 or 128).
  static bool estimate_umeyama_5pt(const float src_kps[10], uint32_t crop_size,
                                   float out_fwd_2x3[6], float out_inv_2x3[6]);

  bool model_ready() const { return m_model_ready; }
  uint32_t model_crop_size() const { return m_model_crop_size; }

private:
  void build_soft_mask(uint32_t crop_size, uint8_t *out_mask_u8);

  void *m_ort_env;
  void *m_ort_session;
  void *m_ort_mem_info;
  char m_img_input_name[64];
  char m_emb_input_name[64];
  char m_output_name[64];
  uint32_t m_model_crop_size;
  bool m_model_ready;

  // Preallocated NCHW float32 tensor buffers for up to 512x512x3
  float *m_input_nchw;
  float *m_output_nchw;
  float *m_mask_scratch_a;
  float *m_mask_scratch_b;
  uint32_t m_cached_mask_size;
};

} // namespace ai
} // namespace camswap
