// face_swap.hpp - Face swap for camswap daemon

#pragma once

#include <stdint.h>

namespace camswap {
namespace ai {

class FaceSwap {
public:
  FaceSwap();
  ~FaceSwap();

  bool init();
  void cleanup();

  bool swap_face(uint8_t *frame, int width, int height);

private:
  void *model_handle;
  int identity_id;
};

} // namespace ai
} // namespace camswap
