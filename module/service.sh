#!/system/bin/sh
# service.sh - Magisk module service script

set -e

MODDIR="${0%/*}"
MODPATH="/data/adb/modules/camswap"

log() {
    echo "[camswap] $1" | logger -t camswap
}

log "Starting camswap daemon..."

# Source device config
DEVICE_ENV="$MODPATH/system/etc/camswap/device.env"
if [ -f "$DEVICE_ENV" ]; then
    . "$DEVICE_ENV"
    log "Device config loaded"
else
    log "WARNING: Device config not found, using defaults"
    lens_policy="all"
    accel_backend="gpu_gl"
    fps_target=20
    fps_floor=15
    rss_budget_mb=350
    thermal_ladder="38:detect_rada,40:roi_128,42:480p,44:passthrough"
fi

# Start the daemon
DAEMON_PATH="$MODPATH/system/bin/camswap_daemon"
if [ -x "$DAEMON_PATH" ]; then
    log "Starting daemon: $DAEMON_PATH"
    # Run daemon in background
    identity_arg=""
    if [ -n "${identity:-}" ]; then
        identity_arg="--identity=$identity"
    fi
    $DAEMON_PATH --device=surfaceduo \
        --lens-policy="$lens_policy" \
        --accel-backend="$accel_backend" \
        --fps-target="$fps_target" \
        --fps-floor="$fps_floor" \
        --rss-budget="$rss_budget_mb" \
        --thermal-ladder="$thermal_ladder" \
        --blur="${blur:-0}" \
        --blur-track="${blur_track:-1}" \
        --blur-level="${blur_level:-40}" \
        $identity_arg &

    log "Daemon started with PID $!"
else
    log "ERROR: Daemon not found or not executable: $DAEMON_PATH"
    exit 1
fi

# Wait for daemon to initialize
sleep 2

# Check if daemon is running
if ps -p $! > /dev/null 2>&1; then
    log "Daemon initialized successfully"
else
    log "ERROR: Daemon failed to start"
    exit 1
fi

log "Service started"
