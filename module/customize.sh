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
mkdir -p "$MODPATH/system/etc/camswap"

# Binaries ship in system/bin. Optional extra copies if a flat layout is used.
log "Copying binaries..."
if [ -f "$MODDIR/payload/camswap_payload" ]; then
    cp -f "$MODDIR/payload/camswap_payload" "$MODPATH/system/bin/"
fi
if [ -f "$MODDIR/daemon/camswap_daemon" ]; then
    cp -f "$MODDIR/daemon/camswap_daemon" "$MODPATH/system/bin/"
fi

# Set permissions
log "Setting permissions..."
chmod 755 "$MODPATH/system/bin/camswap_payload"
chmod 755 "$MODPATH/system/bin/camswap_daemon"

# Copy device config
log "Copying device config..."
if [ -f "$MODDIR/devices/surfaceduo.env" ]; then
    cp -f "$MODDIR/devices/surfaceduo.env" "$MODPATH/system/etc/camswap/device.env"
elif [ ! -f "$MODPATH/system/etc/camswap/device.env" ] && [ -f "$MODDIR/system/etc/camswap/device.env" ]; then
    cp -f "$MODDIR/system/etc/camswap/device.env" "$MODPATH/system/etc/camswap/device.env"
fi

# Copy identity (default)
log "Copying default identity..."
mkdir -p "$MODPATH/system/etc/camswap/identities"
cp -f "$MODDIR/refs/inswapper-512-live/identity_default.bin" "$MODPATH/system/etc/camswap/identities/default.bin" 2>/dev/null || true

log "Installation complete!"
