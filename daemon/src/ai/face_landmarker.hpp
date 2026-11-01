// face_landmarker.hpp - Face landmarker for camswap daemon

#pragma once

#include <stdint.h>

namespace camswap {
namespace ai {

class FaceLandmarker {
public:
  FaceLandmarker();
  ~FaceLandmarker();

  bool init();
  void cleanup();

  bool detect_landmarks(uint8_t *frame, int width, int height);

private:
  void *model_handle;
  int landmark_count;
};

} // namespace ai
} // namespace camswap
