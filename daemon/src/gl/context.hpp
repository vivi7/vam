// context.hpp - GL context and ROI compositor for camswap daemon

#pragma once

#include <stdint.h>

namespace camswap {
namespace gl {

// Initializes headless EGL pbuffer context and compiles ROI compositing shaders.
bool init_context();

// Cleans up EGL context and GL resources.
void cleanup_context();

// Returns true if EGL/GLES context is active and ready.
bool is_ready();

// Composites an aligned swapped RGB patch (crop_size x crop_size) onto an NV12
// frame strictly inside the bounding ROI around the face, using inverse affine
// mapping (dst frame coords -> crop coords) and soft eroded/blurred mask.
// Reads and writes only the ROI sub-rectangle of the NV12 slot.
bool paste_roi_nv12(uint8_t *nv12_frame, uint32_t frame_w, uint32_t frame_h,
                    const uint8_t *swapped_rgb, const uint8_t *mask_u8,
                    uint32_t crop_size, const float fwd_affine_2x3[6],
                    uint32_t roi_x0, uint32_t roi_y0, uint32_t roi_w,
                    uint32_t roi_h);

} // namespace gl
} // namespace camswap
