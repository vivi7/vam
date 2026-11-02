#!/system/bin/sh
# service.sh - Magisk module service script

set -e

MODDIR="${0%/*}"
MODPATH="${MODDIR:-/data/adb/modules/camswap}"

log() {
    /system/bin/log -t camswap "[camswap] $1" 2>/dev/null || echo "[camswap] $1"
}

log "Starting camswap daemon..."

# Source device config
DEVICE_ENV="$MODPATH/system/etc/camswap/device.env"
if [ -f "$DEVICE_ENV" ]; then
    . "$DEVICE_ENV"
    log "Device config loaded from $DEVICE_ENV"
else
    log "WARNING: Device config not found, using defaults"
    device="surfaceduo"
    lens_policy="all"
    accel_backend="gpu_gl"
    fps_target=20
    fps_floor=15
    rss_budget_mb=350
    thermal_ladder="38:detect_rada,40:roi_128,42:480p,44:passthrough"
    ring_path="/data/local/tmp/camswap_ring.shm"
    model_dir="$MODPATH/system/etc/camswap/models"
    landmarker_model="face_landmarker.task"
    swap_model="inswapper_512_live_int8.onnx"
    emap_file="emap.bin"
    identity="$MODPATH/system/etc/camswap/identities/default.bin"
fi

export LD_LIBRARY_PATH="$MODPATH/system/lib64:${LD_LIBRARY_PATH:-/system/lib64}"
export CAMSWAP_RING_PATH="${ring_path:-/data/local/tmp/camswap_ring.shm}"

# Ensure ring parent directory exists before daemon creates and owns the ring
mkdir -p "$(dirname "$CAMSWAP_RING_PATH")" 2>/dev/null || true

DAEMON_PATH="$MODPATH/system/bin/camswap_daemon"
if [ -x "$DAEMON_PATH" ]; then
    log "Starting daemon: $DAEMON_PATH (ring=$CAMSWAP_RING_PATH)"
    "$DAEMON_PATH" \
        --device="${device:-surfaceduo}" \
        --lens-policy="${lens_policy:-all}" \
        --accel-backend="${accel_backend:-gpu_gl}" \
        --fps-target="${fps_target:-20}" \
        --fps-floor="${fps_floor:-15}" \
        --rss-budget="${rss_budget_mb:-350}" \
        --thermal-ladder="${thermal_ladder:-38:detect_rada,40:roi_128,42:480p,44:passthrough}" \
        --ring-path="${ring_path:-/data/local/tmp/camswap_ring.shm}" \
        --model-dir="${model_dir:-$MODPATH/system/etc/camswap/models}" \
        --landmarker-model="${landmarker_model:-face_landmarker.task}" \
        --swap-model="${swap_model:-inswapper_512_live_int8.onnx}" \
        --emap-file="${emap_file:-emap.bin}" \
        --identity="${identity:-$MODPATH/system/etc/camswap/identities/default.bin}" \
        --blur="${blur:-0}" \
        --blur-track="${blur_track:-1}" \
        --blur-level="${blur_level:-40}" </dev/null >/dev/null 2>&1 &

    DAEMON_PID=$!
    log "Daemon started with PID $DAEMON_PID"
else
    log "ERROR: Daemon not found or not executable: $DAEMON_PATH"
    exit 1
fi

# Wait for daemon to initialize and create the file-backed ring
sleep 2

if kill -0 "$DAEMON_PID" 2>/dev/null; then
    log "Daemon initialized successfully"
else
    log "ERROR: Daemon failed to start"
    exit 1
fi

log "Service started"
