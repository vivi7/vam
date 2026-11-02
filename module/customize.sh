#!/system/bin/sh
# customize.sh - Magisk module installation script

set -e

MODDIR="${0%/*}"
MODPATH="${MODPATH:-/data/adb/modules/camswap}"

log() {
    echo "[camswap] $1"
}

log "Starting camswap module installation..."

# Create directories
log "Creating directories..."
mkdir -p "$MODPATH/system/bin"
mkdir -p "$MODPATH/system/lib64"
mkdir -p "$MODPATH/system/etc/camswap/models"
mkdir -p "$MODPATH/system/etc/camswap/identities"

# Copy binaries if flat layout is present
log "Copying binaries..."
if [ -f "$MODDIR/payload/camswap_payload" ]; then
    cp -f "$MODDIR/payload/camswap_payload" "$MODPATH/system/bin/"
fi
if [ -f "$MODDIR/daemon/camswap_daemon" ]; then
    cp -f "$MODDIR/daemon/camswap_daemon" "$MODPATH/system/bin/"
fi

# Copy shared libraries (libonnxruntime.so, payload .so) if present in staging tree
log "Copying shared libraries..."
if [ -d "$MODDIR/lib64" ]; then
    cp -f "$MODDIR/lib64/"*.so "$MODPATH/system/lib64/" 2>/dev/null || true
fi
if [ -f "$MODDIR/system/lib64/libonnxruntime.so" ] && [ ! -f "$MODPATH/system/lib64/libonnxruntime.so" ]; then
    cp -f "$MODDIR/system/lib64/libonnxruntime.so" "$MODPATH/system/lib64/"
fi

# Set permissions
log "Setting permissions..."
[ -f "$MODPATH/system/bin/camswap_payload" ] && chmod 755 "$MODPATH/system/bin/camswap_payload"
[ -f "$MODPATH/system/bin/camswap_daemon" ] && chmod 755 "$MODPATH/system/bin/camswap_daemon"
chmod 755 "$MODPATH/service.sh" 2>/dev/null || true

# Copy device config
log "Copying device config..."
if [ -f "$MODDIR/devices/surfaceduo.env" ]; then
    cp -f "$MODDIR/devices/surfaceduo.env" "$MODPATH/system/etc/camswap/device.env"
elif [ ! -f "$MODPATH/system/etc/camswap/device.env" ] && [ -f "$MODDIR/system/etc/camswap/device.env" ]; then
    cp -f "$MODDIR/system/etc/camswap/device.env" "$MODPATH/system/etc/camswap/device.env"
fi

# Copy AI models (face_landmarker.task, inswapper_512_live_int8.onnx, emap.bin) if present
log "Checking AI models..."
if [ -d "$MODDIR/models" ]; then
    cp -f "$MODDIR/models/"* "$MODPATH/system/etc/camswap/models/" 2>/dev/null || true
fi

# Copy default identity if present
log "Checking default identity..."
if [ -f "$MODDIR/identities/default.bin" ]; then
    cp -f "$MODDIR/identities/default.bin" "$MODPATH/system/etc/camswap/identities/default.bin"
elif [ -f "$MODDIR/refs/inswapper-512-live/identity_default.bin" ]; then
    cp -f "$MODDIR/refs/inswapper-512-live/identity_default.bin" "$MODPATH/system/etc/camswap/identities/default.bin"
fi

log "Installation complete (missing models/identity will run in safe passthrough)."
