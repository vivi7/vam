// main.cpp - Entry point for camswap daemon

#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ai/pipeline.hpp"
#include "common/logging.hpp"
#include "common/ringbuffer.hpp"
#include "ring/ringbuffer.hpp"

static volatile bool g_running = true;

static void signal_handler(int sig) {
  LOGI("daemon: received signal %d, shutting down", sig);
  g_running = false;
}

static uint64_t monotonic_us() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s [options]\n", prog);
  fprintf(stderr, "Options:\n");
  fprintf(stderr,
          "  -d, --device <name>            Device name (default: surfaceduo)\n");
  fprintf(stderr,
          "  -l, --lens-policy <p>          Lens policy: all|front|back "
          "(default: all)\n");
  fprintf(stderr,
          "  -a, --accel-backend <b>        Acceleration: gpu_gl (default: "
          "gpu_gl)\n");
  fprintf(stderr,
          "  -f, --fps-target <n>           Target FPS (default: 20)\n");
  fprintf(stderr,
          "  -m, --fps-floor <n>            Minimum FPS (default: 15)\n");
  fprintf(stderr,
          "  -r, --rss-budget <n>           RSS budget in MB (default: 350)\n");
  fprintf(stderr,
          "  -t, --thermal-ladder <l>       Thermal ladder\n");
  fprintf(stderr,
          "  -R, --ring-path <path>         File-backed ring buffer path\n");
  fprintf(stderr,
          "  -M, --model-dir <dir>          Directory containing AI models\n");
  fprintf(stderr,
          "  -F, --landmarker-model <file>  Landmarker model file\n");
  fprintf(stderr,
          "  -W, --swap-model <file>        INSwapper ONNX model file\n");
  fprintf(stderr,
          "  -E, --emap-file <file>         EMAP binary matrix file\n");
  fprintf(stderr,
          "  -i, --identity <path>          Identity embedding file\n");
  fprintf(stderr,
          "  -B, --blur <0|1>               Blur tracked face\n");
  fprintf(stderr,
          "  -K, --blur-track <0|1>         Keep blur on tracked face\n");
  fprintf(stderr,
          "  -L, --blur-level <0-100>       Blur strength\n");
  fprintf(stderr,
          "  -S, --selftest                 Run synthetic NV12 slot self-test "
          "and exit\n");
  fprintf(stderr,
          "  -h, --help                     Show this help\n");
}

static int run_selftest(camswap::ring::RingBuffer &ring,
                        camswap::ai::Pipeline &pipeline) {
  const uint32_t w = 640;
  const uint32_t h = 480;
  const uint32_t frame_bytes = camswap::ring_frame_bytes(w, h);

  LOGI("selftest: publishing synthetic %ux%u NV12 slot (%u bytes)", w, h,
       frame_bytes);
  if (!ring.publish_synthetic_nv12(w, h, camswap::kFormatImplDefined)) {
    LOGE("selftest: failed to publish synthetic NV12 frame");
    return 1;
  }

  camswap::RingHeader *hdr = ring.header();
  if (!hdr || hdr->magic != camswap::kRingMagic ||
      hdr->version != camswap::kRingVersion) {
    LOGE("selftest: ring header invalid");
    return 1;
  }

  // Snapshot original synthetic frame when in passthrough so we can verify
  // zero partial/hybrid writes occurred
  const camswap::RingLane &lane0 = hdr->lane[0];
  const uint8_t *slot0_ptr = ring.base() + lane0.dataOffset;
  uint8_t *snapshot = static_cast<uint8_t *>(malloc(frame_bytes));
  if (!snapshot) {
    LOGE("selftest: malloc snapshot failed");
    return 1;
  }
  memcpy(snapshot, slot0_ptr, frame_bytes);

  const int thermal_c = ring.get_temperature();
  const uint64_t t0 = monotonic_us();
  uint32_t processed = ring.process(thermal_c);
  const uint64_t elapsed_us = monotonic_us() - t0;

  if (processed != 1 || lane0.consumed != lane0.published ||
      lane0.slots[0].consumed != lane0.published) {
    LOGE("selftest: slot consumption invariant failed (processed=%u, "
         "published=%llu, consumed=%llu)",
         processed, static_cast<unsigned long long>(lane0.published),
         static_cast<unsigned long long>(lane0.consumed));
    free(snapshot);
    return 1;
  }

  const uint32_t flags = lane0.slots[0].flags;
  const bool is_swapped = (flags & camswap::kSlotFlagSwapped) != 0;
  const bool is_passthrough = (flags & camswap::kSlotFlagPassthrough) != 0;
  if (is_swapped == is_passthrough) {
    LOGE("selftest: ambiguous slot flags=0x%x (never hybrid)", flags);
    free(snapshot);
    return 1;
  }

  if (is_passthrough) {
    // Verify not a single byte of the NV12 slot was modified in passthrough
    if (memcmp(snapshot, slot0_ptr, frame_bytes) != 0) {
      LOGE("selftest: passthrough mode modified slot pixels (hybrid frame!)");
      free(snapshot);
      return 1;
    }
  }

  free(snapshot);

  const char *mode_str = is_swapped ? "swap" : "passthrough";
  LOGI("selftest: PASS mode=%s ready_for_swap=%d thermal=%dC elapsed=%llu us "
       "published=%llu consumed=%llu dropped=%llu",
       mode_str, pipeline.is_ready_for_swap() ? 1 : 0, thermal_c,
       static_cast<unsigned long long>(elapsed_us),
       static_cast<unsigned long long>(hdr->published),
       static_cast<unsigned long long>(hdr->consumed),
       static_cast<unsigned long long>(hdr->dropped));
  fprintf(stdout,
          "SELFTEST PASS: mode=%s thermal=%dC elapsed_us=%llu "
          "published=%llu consumed=%llu\n",
          mode_str, thermal_c, static_cast<unsigned long long>(elapsed_us),
          static_cast<unsigned long long>(hdr->published),
          static_cast<unsigned long long>(hdr->consumed));
  return 0;
}

int main(int argc, char **argv) {
  LOGI("daemon: starting camswap daemon");

  const char *device = "surfaceduo";
  const char *lens_policy = "all";
  const char *accel_backend = "gpu_gl";
  int fps_target = 20;
  int fps_floor = 15;
  int rss_budget = 350;
  const char *thermal_ladder =
      "38:detect_rada,40:roi_128,42:480p,44:passthrough";
  const char *ring_path = CAMSWAP_DEFAULT_RING_PATH;
  const char *model_dir =
      "/data/adb/modules/camswap/system/etc/camswap/models";
  const char *landmarker_model = "face_landmarker.task";
  const char *swap_model = "inswapper_512_live_int8.onnx";
  const char *emap_file = "emap.bin";
  const char *identity =
      "/data/adb/modules/camswap/system/etc/camswap/identities/default.bin";
  int blur = 0;
  int blur_track = 1;
  int blur_level = 40;
  bool selftest = false;

  static struct option long_options[] = {
      {"device", required_argument, 0, 'd'},
      {"lens-policy", required_argument, 0, 'l'},
      {"accel-backend", required_argument, 0, 'a'},
      {"fps-target", required_argument, 0, 'f'},
      {"fps-floor", required_argument, 0, 'm'},
      {"rss-budget", required_argument, 0, 'r'},
      {"thermal-ladder", required_argument, 0, 't'},
      {"ring-path", required_argument, 0, 'R'},
      {"model-dir", required_argument, 0, 'M'},
      {"landmarker-model", required_argument, 0, 'F'},
      {"swap-model", required_argument, 0, 'W'},
      {"emap-file", required_argument, 0, 'E'},
      {"identity", required_argument, 0, 'i'},
      {"blur", required_argument, 0, 'B'},
      {"blur-track", required_argument, 0, 'K'},
      {"blur-level", required_argument, 0, 'L'},
      {"selftest", no_argument, 0, 'S'},
      {"help", no_argument, 0, 'h'},
      {0, 0, 0, 0}};

  int option_index = 0;
  int c;
  while ((c = getopt_long(argc, argv, "d:l:a:f:m:r:t:R:M:F:W:E:i:B:K:L:Sh",
                          long_options, &option_index)) != -1) {
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
    case 'R':
      ring_path = optarg;
      break;
    case 'M':
      model_dir = optarg;
      break;
    case 'F':
      landmarker_model = optarg;
      break;
    case 'W':
      swap_model = optarg;
      break;
    case 'E':
      emap_file = optarg;
      break;
    case 'i':
      identity = optarg;
      break;
    case 'B':
      blur = atoi(optarg);
      break;
    case 'K':
      blur_track = atoi(optarg);
      break;
    case 'L':
      blur_level = atoi(optarg);
      break;
    case 'S':
      selftest = true;
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  LOGI("daemon: device=%s lens_policy=%s accel_backend=%s", device, lens_policy,
       accel_backend);
  LOGI("daemon: fps_target=%d fps_floor=%d rss_budget=%dMB", fps_target,
       fps_floor, rss_budget);
  LOGI("daemon: thermal_ladder=%s", thermal_ladder);
  LOGI("daemon: ring_path=%s model_dir=%s", ring_path, model_dir);
  LOGI("daemon: landmarker=%s swap=%s emap=%s identity=%s", landmarker_model,
       swap_model, emap_file, identity[0] ? identity : "(none)");

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);

  camswap::ai::PipelineConfig pcfg{};
  pcfg.device = device;
  pcfg.lens_policy = lens_policy;
  pcfg.accel_backend = accel_backend;
  pcfg.fps_target = fps_target;
  pcfg.fps_floor = fps_floor;
  pcfg.rss_budget_mb = rss_budget;
  pcfg.thermal_ladder = thermal_ladder;
  pcfg.model_dir = model_dir;
  pcfg.landmarker_model = landmarker_model;
  pcfg.swap_model = swap_model;
  pcfg.emap_file = emap_file;
  pcfg.identity_path = identity;
  pcfg.blur = blur;
  pcfg.blur_track = blur_track;
  pcfg.blur_level = blur_level;

  camswap::ai::Pipeline pipeline;
  if (!pipeline.init(pcfg)) {
    LOGE("daemon: failed to allocate pipeline scratch buffers");
    return 1;
  }

  camswap::ring::RingBuffer ring;
  if (!ring.init(ring_path, &pipeline)) {
    LOGE("daemon: failed to initialize file-backed ring buffer at %s",
         ring_path);
    pipeline.cleanup();
    return 1;
  }

  if (selftest) {
    int rc = run_selftest(ring, pipeline);
    ring.cleanup();
    pipeline.cleanup();
    return rc;
  }

  LOGI("daemon: ready and owning ring at %s", ring_path);
  bool logged_thermal_passthrough = false;

  while (g_running) {
    int temp_c = ring.get_temperature();
    int pass_c = pipeline.thresholds().passthrough_c;
    if (temp_c >= pass_c) {
      if (!logged_thermal_passthrough) {
        LOGW("daemon: thermal threshold reached (%dC >= %dC), switching to "
             "passthrough (daemon stays running)",
             temp_c, pass_c);
        logged_thermal_passthrough = true;
      }
    } else if (logged_thermal_passthrough && temp_c <= (pass_c - 2)) {
      LOGI("daemon: temperature cooled to %dC, resuming normal ladder", temp_c);
      logged_thermal_passthrough = false;
    }

    uint32_t n = ring.process(temp_c);
    if (n == 0) {
      usleep(2000); // 2ms idle poll
    }
  }

  LOGI("daemon: shutting down");
  ring.cleanup();
  pipeline.cleanup();
  LOGI("daemon: shutdown complete");
  return 0;
}
