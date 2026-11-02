// context.cpp - Headless EGL pbuffer context and GLES2 ROI compositor

#include "context.hpp"
#include "../common/logging.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <string.h>

namespace camswap {
namespace gl {

namespace {

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLSurface g_surface = EGL_NO_SURFACE;
static bool g_ready = false;

static GLuint g_program = 0;
static GLuint g_fbo = 0;
static GLuint g_tex_fbo = 0;
static GLuint g_tex_bg = 0;
static GLuint g_tex_swap = 0;
static GLuint g_tex_mask = 0;

static GLint g_loc_pos = -1;
static GLint g_loc_uv = -1;
static GLint g_loc_bg = -1;
static GLint g_loc_swap = -1;
static GLint g_loc_mask = -1;
static GLint g_loc_roi_origin = -1;
static GLint g_loc_roi_size = -1;
static GLint g_loc_fwd_row0 = -1;
static GLint g_loc_fwd_row1 = -1;
static GLint g_loc_crop_size = -1;

// Maximum ROI texture dimension preallocated once
static const uint32_t kMaxRoiDim = 640;

// Fixed scratch buffers for ROI RGB upload/readback (zero per-frame allocs)
alignas(16) static uint8_t g_roi_bg_rgba[kMaxRoiDim * kMaxRoiDim * 4];
alignas(16) static uint8_t g_roi_out_rgba[kMaxRoiDim * kMaxRoiDim * 4];

static const char *kVertexShaderSrc =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  vUV = aUV;\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *kFragmentShaderSrc =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uBgTex;\n"
    "uniform sampler2D uSwapTex;\n"
    "uniform sampler2D uMaskTex;\n"
    "uniform vec2 uRoiOrigin;\n"
    "uniform vec2 uRoiSize;\n"
    "uniform vec3 uFwdRow0;\n"
    "uniform vec3 uFwdRow1;\n"
    "uniform float uCropSize;\n"
    "void main() {\n"
    "  vec4 bg = texture2D(uBgTex, vUV);\n"
    "  vec2 frameXY = uRoiOrigin + vUV * uRoiSize;\n"
    "  float cx = (uFwdRow0.x * frameXY.x + uFwdRow0.y * frameXY.y + uFwdRow0.z) / uCropSize;\n"
    "  float cy = (uFwdRow1.x * frameXY.x + uFwdRow1.y * frameXY.y + uFwdRow1.z) / uCropSize;\n"
    "  if (cx < 0.0 || cx > 1.0 || cy < 0.0 || cy > 1.0) {\n"
    "    gl_FragColor = bg;\n"
    "    return;\n"
    "  }\n"
    "  vec2 cropUV = vec2(cx, cy);\n"
    "  vec4 sw = texture2D(uSwapTex, cropUV);\n"
    "  float a = texture2D(uMaskTex, cropUV).r;\n"
    "  gl_FragColor = vec4(mix(bg.rgb, sw.rgb, a), 1.0);\n"
    "}\n";

GLuint compile_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  if (!s)
    return 0;
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[256] = {0};
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    LOGW("gl: shader compile failed: %s", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint create_tex_2d() {
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

inline uint8_t clamp_u8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t &r, uint8_t &g,
                       uint8_t &b) {
  int c = static_cast<int>(y) - 16;
  int d = static_cast<int>(u) - 128;
  int e = static_cast<int>(v) - 128;
  if (c < 0)
    c = 0;
  r = clamp_u8((298 * c + 409 * e + 128) >> 8);
  g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
  b = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

inline void rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t &y, uint8_t &u,
                       uint8_t &v) {
  int ri = r, gi = g, bi = b;
  y = clamp_u8(((66 * ri + 129 * gi + 25 * bi + 128) >> 8) + 16);
  u = clamp_u8(((-38 * ri - 74 * gi + 112 * bi + 128) >> 8) + 128);
  v = clamp_u8(((112 * ri - 94 * gi - 18 * bi + 128) >> 8) + 128);
}

// Software fallback matching exact GL shader equations when running in
// headless/host selftest environments where EGL driver is unavailable.
bool paste_roi_software(uint8_t *nv12_frame, uint32_t frame_w, uint32_t frame_h,
                        const uint8_t *swapped_rgb, const uint8_t *mask_u8,
                        uint32_t crop_size, const float fwd[6], uint32_t roi_x0,
                        uint32_t roi_y0, uint32_t roi_w, uint32_t roi_h) {
  uint8_t *y_plane = nv12_frame;
  uint8_t *uv_plane = nv12_frame + frame_w * frame_h;

  for (uint32_t ry = 0; ry < roi_h; ++ry) {
    uint32_t fy = roi_y0 + ry;
    if (fy >= frame_h)
      break;
    for (uint32_t rx = 0; rx < roi_w; ++rx) {
      uint32_t fx = roi_x0 + rx;
      if (fx >= frame_w)
        break;

      float cx = fwd[0] * static_cast<float>(fx) +
                 fwd[1] * static_cast<float>(fy) + fwd[2];
      float cy = fwd[3] * static_cast<float>(fx) +
                 fwd[4] * static_cast<float>(fy) + fwd[5];

      int icx = static_cast<int>(cx + 0.5f);
      int icy = static_cast<int>(cy + 0.5f);
      if (icx < 0 || icx >= static_cast<int>(crop_size) || icy < 0 ||
          icy >= static_cast<int>(crop_size)) {
        continue;
      }

      uint8_t alpha_u8 = mask_u8[icy * crop_size + icx];
      if (alpha_u8 <= 2)
        continue;

      uint32_t uv_idx = (fy / 2) * frame_w + (fx & ~1u);
      uint8_t orig_y = y_plane[fy * frame_w + fx];
      uint8_t orig_u = uv_plane[uv_idx];
      uint8_t orig_v = uv_plane[uv_idx + 1];

      uint8_t bg_r, bg_g, bg_b;
      yuv_to_rgb(orig_y, orig_u, orig_v, bg_r, bg_g, bg_b);

      const uint8_t *sw = &swapped_rgb[(icy * crop_size + icx) * 3];
      uint32_t a = alpha_u8;
      uint32_t inv_a = 255u - a;

      uint8_t out_r = static_cast<uint8_t>((sw[0] * a + bg_r * inv_a) / 255u);
      uint8_t out_g = static_cast<uint8_t>((sw[1] * a + bg_g * inv_a) / 255u);
      uint8_t out_b = static_cast<uint8_t>((sw[2] * a + bg_b * inv_a) / 255u);

      uint8_t ny, nu, nv;
      rgb_to_yuv(out_r, out_g, out_b, ny, nu, nv);
      y_plane[fy * frame_w + fx] = ny;
      if (((fx & 1u) == 0) && ((fy & 1u) == 0)) {
        uv_plane[uv_idx] = nu;
        uv_plane[uv_idx + 1] = nv;
      }
    }
  }
  return true;
}

} // namespace

bool init_context() {
  LOGI("gl: initializing headless EGL pbuffer context");

  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY) {
    LOGW("gl: eglGetDisplay returned EGL_NO_DISPLAY, using software ROI fallback");
    g_ready = false;
    return true;
  }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(g_display, &major, &minor)) {
    LOGW("gl: eglInitialize failed, using software ROI fallback");
    g_display = EGL_NO_DISPLAY;
    g_ready = false;
    return true;
  }

  const EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                                   EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE,
                                   EGL_OPENGL_ES2_BIT,
                                   EGL_RED_SIZE,
                                   8,
                                   EGL_GREEN_SIZE,
                                   8,
                                   EGL_BLUE_SIZE,
                                   8,
                                   EGL_ALPHA_SIZE,
                                   8,
                                   EGL_NONE};

  EGLConfig config = nullptr;
  EGLint num_configs = 0;
  if (!eglChooseConfig(g_display, config_attribs, &config, 1, &num_configs) ||
      num_configs < 1) {
    LOGW("gl: eglChooseConfig failed, using software ROI fallback");
    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
    g_ready = false;
    return true;
  }

  const EGLint pbuffer_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  g_surface = eglCreatePbufferSurface(g_display, config, pbuffer_attribs);
  if (g_surface == EGL_NO_SURFACE) {
    LOGW("gl: eglCreatePbufferSurface failed, using software ROI fallback");
    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
    g_ready = false;
    return true;
  }

  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctx_attribs);
  if (g_context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
    LOGW("gl: eglCreateContext/MakeCurrent failed, using software ROI fallback");
    cleanup_context();
    g_ready = false;
    return true;
  }

  GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShaderSrc);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentShaderSrc);
  if (!vs || !fs) {
    if (vs)
      glDeleteShader(vs);
    if (fs)
      glDeleteShader(fs);
    cleanup_context();
    return true;
  }

  g_program = glCreateProgram();
  glAttachShader(g_program, vs);
  glAttachShader(g_program, fs);
  glLinkProgram(g_program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = 0;
  glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
  if (!linked) {
    LOGW("gl: program link failed, using software ROI fallback");
    cleanup_context();
    return true;
  }

  g_loc_pos = glGetAttribLocation(g_program, "aPos");
  g_loc_uv = glGetAttribLocation(g_program, "aUV");
  g_loc_bg = glGetUniformLocation(g_program, "uBgTex");
  g_loc_swap = glGetUniformLocation(g_program, "uSwapTex");
  g_loc_mask = glGetUniformLocation(g_program, "uMaskTex");
  g_loc_roi_origin = glGetUniformLocation(g_program, "uRoiOrigin");
  g_loc_roi_size = glGetUniformLocation(g_program, "uRoiSize");
  g_loc_fwd_row0 = glGetUniformLocation(g_program, "uFwdRow0");
  g_loc_fwd_row1 = glGetUniformLocation(g_program, "uFwdRow1");
  g_loc_crop_size = glGetUniformLocation(g_program, "uCropSize");

  g_tex_fbo = create_tex_2d();
  g_tex_bg = create_tex_2d();
  g_tex_swap = create_tex_2d();
  g_tex_mask = create_tex_2d();

  glGenFramebuffers(1, &g_fbo);
  g_ready = true;
  LOGI("gl: EGL %d.%d pbuffer context and ROI shader initialized", major, minor);
  return true;
}

bool is_ready() { return g_ready; }

void cleanup_context() {
  LOGI("gl: cleaning up GL context");
  if (g_display != EGL_NO_DISPLAY && g_context != EGL_NO_CONTEXT) {
    eglMakeCurrent(g_display, g_surface, g_surface, g_context);
    if (g_fbo) {
      glDeleteFramebuffers(1, &g_fbo);
      g_fbo = 0;
    }
    GLuint texs[4] = {g_tex_fbo, g_tex_bg, g_tex_swap, g_tex_mask};
    glDeleteTextures(4, texs);
    g_tex_fbo = g_tex_bg = g_tex_swap = g_tex_mask = 0;
    if (g_program) {
      glDeleteProgram(g_program);
      g_program = 0;
    }
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (g_display != EGL_NO_DISPLAY) {
    if (g_context != EGL_NO_CONTEXT) {
      eglDestroyContext(g_display, g_context);
      g_context = EGL_NO_CONTEXT;
    }
    if (g_surface != EGL_NO_SURFACE) {
      eglDestroySurface(g_display, g_surface);
      g_surface = EGL_NO_SURFACE;
    }
    eglTerminate(g_display);
    g_display = EGL_NO_DISPLAY;
  }
  g_ready = false;
}

bool paste_roi_nv12(uint8_t *nv12_frame, uint32_t frame_w, uint32_t frame_h,
                    const uint8_t *swapped_rgb, const uint8_t *mask_u8,
                    uint32_t crop_size, const float fwd_affine_2x3[6],
                    uint32_t roi_x0, uint32_t roi_y0, uint32_t roi_w,
                    uint32_t roi_h) {
  if (!nv12_frame || !swapped_rgb || !mask_u8 || roi_w == 0 || roi_h == 0) {
    return false;
  }

  // Clamp ROI to even coordinates and maximum preallocated texture bounds
  roi_x0 &= ~1u;
  roi_y0 &= ~1u;
  if (roi_x0 >= frame_w || roi_y0 >= frame_h) {
    return false;
  }
  if (roi_x0 + roi_w > frame_w)
    roi_w = frame_w - roi_x0;
  if (roi_y0 + roi_h > frame_h)
    roi_h = frame_h - roi_y0;
  roi_w &= ~1u;
  roi_h &= ~1u;

  if (roi_w == 0 || roi_h == 0 || roi_w > kMaxRoiDim || roi_h > kMaxRoiDim) {
    return paste_roi_software(nv12_frame, frame_w, frame_h, swapped_rgb,
                              mask_u8, crop_size, fwd_affine_2x3, roi_x0,
                              roi_y0, roi_w, roi_h);
  }

  if (!g_ready) {
    return paste_roi_software(nv12_frame, frame_w, frame_h, swapped_rgb,
                              mask_u8, crop_size, fwd_affine_2x3, roi_x0,
                              roi_y0, roi_w, roi_h);
  }

  // Extract ONLY the ROI sub-rectangle from NV12 to RGBA (never the full frame)
  const uint8_t *y_plane = nv12_frame;
  const uint8_t *uv_plane = nv12_frame + frame_w * frame_h;
  for (uint32_t ry = 0; ry < roi_h; ++ry) {
    uint32_t fy = roi_y0 + ry;
    for (uint32_t rx = 0; rx < roi_w; ++rx) {
      uint32_t fx = roi_x0 + rx;
      uint32_t uv_idx = (fy / 2) * frame_w + (fx & ~1u);
      uint8_t r, g, b;
      yuv_to_rgb(y_plane[fy * frame_w + fx], uv_plane[uv_idx],
                 uv_plane[uv_idx + 1], r, g, b);
      uint8_t *dst = &g_roi_bg_rgba[(ry * roi_w + rx) * 4];
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
      dst[3] = 255;
    }
  }

  // Render ROI composition on GPU
  glBindTexture(GL_TEXTURE_2D, g_tex_fbo);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(roi_w),
               static_cast<GLsizei>(roi_h), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);

  glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         g_tex_fbo, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return paste_roi_software(nv12_frame, frame_w, frame_h, swapped_rgb,
                              mask_u8, crop_size, fwd_affine_2x3, roi_x0,
                              roi_y0, roi_w, roi_h);
  }

  glViewport(0, 0, static_cast<GLsizei>(roi_w), static_cast<GLsizei>(roi_h));
  glUseProgram(g_program);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_tex_bg);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(roi_w),
               static_cast<GLsizei>(roi_h), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               g_roi_bg_rgba);
  glUniform1i(g_loc_bg, 0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, g_tex_swap);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, static_cast<GLsizei>(crop_size),
               static_cast<GLsizei>(crop_size), 0, GL_RGB, GL_UNSIGNED_BYTE,
               swapped_rgb);
  glUniform1i(g_loc_swap, 1);

  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, g_tex_mask);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, static_cast<GLsizei>(crop_size),
               static_cast<GLsizei>(crop_size), 0, GL_LUMINANCE,
               GL_UNSIGNED_BYTE, mask_u8);
  glUniform1i(g_loc_mask, 2);

  glUniform2f(g_loc_roi_origin, static_cast<float>(roi_x0),
              static_cast<float>(roi_y0));
  glUniform2f(g_loc_roi_size, static_cast<float>(roi_w),
              static_cast<float>(roi_h));
  glUniform3f(g_loc_fwd_row0, fwd_affine_2x3[0], fwd_affine_2x3[1],
              fwd_affine_2x3[2]);
  glUniform3f(g_loc_fwd_row1, fwd_affine_2x3[3], fwd_affine_2x3[4],
              fwd_affine_2x3[5]);
  glUniform1f(g_loc_crop_size, static_cast<float>(crop_size));

  static const GLfloat kQuadPos[] = {-1.0f, -1.0f, 1.0f, -1.0f,
                                     -1.0f, 1.0f,  1.0f, 1.0f};
  static const GLfloat kQuadUV[] = {0.0f, 0.0f, 1.0f, 0.0f,
                                    0.0f, 1.0f, 1.0f, 1.0f};

  glEnableVertexAttribArray(static_cast<GLuint>(g_loc_pos));
  glVertexAttribPointer(static_cast<GLuint>(g_loc_pos), 2, GL_FLOAT, GL_FALSE,
                        0, kQuadPos);
  glEnableVertexAttribArray(static_cast<GLuint>(g_loc_uv));
  glVertexAttribPointer(static_cast<GLuint>(g_loc_uv), 2, GL_FLOAT, GL_FALSE, 0,
                        kQuadUV);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glReadPixels(0, 0, static_cast<GLsizei>(roi_w), static_cast<GLsizei>(roi_h),
               GL_RGBA, GL_UNSIGNED_BYTE, g_roi_out_rgba);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Convert composited ROI RGBA back into the NV12 ROI sub-rectangle
  uint8_t *y_out = nv12_frame;
  uint8_t *uv_out = nv12_frame + frame_w * frame_h;
  for (uint32_t ry = 0; ry < roi_h; ++ry) {
    uint32_t fy = roi_y0 + ry;
    for (uint32_t rx = 0; rx < roi_w; ++rx) {
      uint32_t fx = roi_x0 + rx;
      const uint8_t *px = &g_roi_out_rgba[(ry * roi_w + rx) * 4];
      uint8_t ny, nu, nv;
      rgb_to_yuv(px[0], px[1], px[2], ny, nu, nv);
      y_out[fy * frame_w + fx] = ny;
      if (((fx & 1u) == 0) && ((fy & 1u) == 0)) {
        uint32_t uv_idx = (fy / 2) * frame_w + fx;
        uv_out[uv_idx] = nu;
        uv_out[uv_idx + 1] = nv;
      }
    }
  }

  return true;
}

} // namespace gl
} // namespace camswap
