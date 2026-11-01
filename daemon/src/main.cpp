// main.cpp - Entry point for camswap daemon

#include <android/log.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai/face_landmarker.hpp"
#include "ai/face_swap.hpp"
#include "common/logging.hpp"
#include "common/ringbuffer.hpp"
#include "gl/context.hpp"
#include "ring/ringbuffer.hpp"

#define LOG_TAG "CamswapDaemon"

static volatile bool g_running = true;

static void signal_handler(int sig) {
  LOGI("daemon: received signal %d, shutting down", sig);
  g_running = false;
}

static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s [options]\n", prog);
  fprintf(stderr, "Options:\n");
  fprintf(stderr,
          "  -d, --device <name>      Device name (default: surfaceduo)\n");
  fprintf(stderr, "  -l, --lens-policy <p>    Lens policy: all|front|back "
                  "(default: all)\n");
  fprintf(stderr, "  -a, --accel-backend <b>  Acceleration: gpu_gl|cpu "
                  "(default: gpu_gl)\n");
  fprintf(stderr, "  -f, --fps-target <n>     Target FPS (default: 20)\n");
  fprintf(stderr, "  -m, --fps-floor <n>      Minimum FPS (default: 15)\n");
  fprintf(stderr,
          "  -r, --rss-budget <n>     RSS budget in MB (default: 350)\n");
  fprintf(stderr, "  -t, --thermal-ladder <l> Thermal ladder (default: "
                  "38:detect_rada,40:roi_128,42:480p,44:passthrough)\n");
  fprintf(stderr, "  -h, --help               Show this help\n");
}

int main(int argc, char **argv) {
  LOGI("daemon: starting camswap daemon");

  // Default options
  const char *device = "surfaceduo";
  const char *lens_policy = "all";
  const char *accel_backend = "gpu_gl";
  int fps_target = 20;
  int fps_floor = 15;
  int rss_budget = 350;
  const char *thermal_ladder =
      "38:detect_rada,40:roi_128,42:480p,44:passthrough";

  // Parse options
  static struct option long_options[] = {
      {"device", required_argument, 0, 'd'},
      {"lens-policy", required_argument, 0, 'l'},
      {"accel-backend", required_argument, 0, 'a'},
      {"fps-target", required_argument, 0, 'f'},
      {"fps-floor", required_argument, 0, 'm'},
      {"rss-budget", required_argument, 0, 'r'},
      {"thermal-ladder", required_argument, 0, 't'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}};

  int option_index = 0;
  int c;
  while ((c = getopt_long(argc, argv, "d:l:a:f:m:r:t:h", long_options,
                          &option_index)) != -1) {
    switch (c) {
    case 'd':
      device = optarg;
      break;
    case 'l':
      lens_policy = optarg;
      break;
    case 'a':
      accel_backend = optarg;
      break;
    case 'f':
      fps_target = atoi(optarg);
      break;
    case 'm':
      fps_floor = atoi(optarg);
      break;
    case 'r':
      rss_budget = atoi(optarg);
      break;
    case 't':
      thermal_ladder = optarg;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  LOGI("daemon: device=%s", device);
  LOGI("daemon: lens_policy=%s", lens_policy);
  LOGI("daemon: accel_backend=%s", accel_backend);
  LOGI("daemon: fps_target=%d", fps_target);
  LOGI("daemon: fps_floor=%d", fps_floor);
  LOGI("daemon: rss_budget=%d MB", rss_budget);
  LOGI("daemon: thermal_ladder=%s", thermal_ladder);

  // Setup signal handlers
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);

  // Initialize GL context
  LOGI("daemon: initializing GL context");
  if (!camswap::gl::init_context()) {
    LOGE("daemon: failed to initialize GL context");
    return 1;
  }

  // Initialize face landmarker
  LOGI("daemon: initializing face landmarker");
  camswap::ai::FaceLandmarker landmarker;
  if (!landmarker.init()) {
    LOGE("daemon: failed to initialize face landmarker");
    camswap::gl::cleanup_context();
    return 1;
  }

  // Initialize face swap
  LOGI("daemon: initializing face swap");
  camswap::ai::FaceSwap faceswap;
  if (!faceswap.init()) {
    LOGE("daemon: failed to initialize face swap");
    landmarker.cleanup();
    camswap::gl::cleanup_context();
    return 1;
  }

  // Initialize ring buffer
  LOGI("daemon: initializing ring buffer");
  camswap::ring::RingBuffer ring;
  if (!ring.init()) {
    LOGE("daemon: failed to initialize ring buffer");
    faceswap.cleanup();
    landmarker.cleanup();
    camswap::gl::cleanup_context();
    return 1;
  }

  LOGI("daemon: daemon ready");

  // Main loop
  while (g_running) {
    // Process frames from ring buffer
    ring.process();

    // Check thermal
    int temp = ring.get_temperature();
    if (temp >= 44) {
      LOGW("daemon: thermal shutdown, temp=%d", temp);
      break;
    }

    // Sleep to avoid busy loop
    usleep(10000); // 10ms
  }

  LOGI("daemon: shutting down");

  // Cleanup
  ring.cleanup();
  faceswap.cleanup();
  landmarker.cleanup();
  camswap::gl::cleanup_context();

  LOGI("daemon: shutdown complete");
  return 0;
}
