// face_landmarker.hpp - Face landmarker and SCRFD fallback for camswap daemon

#pragma once

#include <stdint.h>

namespace camswap {
namespace ai {

struct FaceBox {
  float x1;
  float y1;
  float x2;
  float y2;
  float score;
  // 5 canonical InsightFace keypoints:
  // [0,1]=left_eye, [2,3]=right_eye, [4,5]=nose, [6,7]=left_mouth, [8,9]=right_mouth
  float kps[10];
};

class FaceLandmarker {
public:
  FaceLandmarker();
  ~FaceLandmarker();

  // Initializes MediaPipe Face Landmarker (TFLite GPU delegate) or SCRFD ONNX
  // fallback in the shared ORT runtime. Never aborts if files are missing:
  // marks model_ready() == false so the pipeline falls back to passthrough.
  bool init(const char *model_path = nullptr,
            const char *fallback_scrfd_path = nullptr);
  void cleanup();

  // Runs detection on an NV12 frame (downsampled into preallocated 640x640
  // scratch) and returns true if a valid face with 5 landmarks was found.
  bool detect_landmarks(const uint8_t *nv12_frame, int width, int height,
                        FaceBox *out_face);

  // Lightweight tracking step for intermediate frames (between full detections
  // every 8 frames): refines landmark/box center from luma centroid in ROI and
  // applies exponential smoothing.
  bool track_landmarks(const uint8_t *nv12_frame, int width, int height,
                       const FaceBox &prev_face, FaceBox *out_face);

  bool model_ready() const { return m_model_ready; }

private:
  bool run_scrfd_ort(const uint8_t *nv12_frame, int width, int height,
                     FaceBox *out_face);

  void *m_tflite_lib;
  void *m_ort_env;
  void *m_ort_session;
  void *m_ort_mem_info;
  char m_input_name[64];
  char m_output_names[10][64];
  int m_output_count;
  bool m_model_ready;
  bool m_use_ort_scrfd;

  // Preallocated 640x640 NCHW float32 input tensor buffer (zero per-frame alloc)
  float *m_det_input_nchw;
};

} // namespace ai
} // namespace camswap
