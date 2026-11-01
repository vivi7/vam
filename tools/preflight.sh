#!/bin/bash
# preflight.sh - Device compatibility verification for camswap
# Verifies device, SDK, build, ABI, and camera capabilities

set -e

LOGFILE="/data/local/tmp/preflight.log"
DEVICES_DIR="/data/local/tmp/devices"
REFS_DIR="/data/local/tmp/refs"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "$1" | tee -a "$LOGFILE"
}

log_header() {
    log "${YELLOW}========================================${NC}"
    log "${YELLOW}$1${NC}"
    log "${YELLOW}========================================${NC}"
}

# Check if running as root
if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root${NC}"
    exit 1
fi

# Clear previous log
> "$LOGFILE"

log_header "CAMSWAP PREFLIGHT CHECK"
log "Timestamp: $(date -Iseconds)"
log ""

# ============================================================================
# 1. DEVICE VERIFICATION
# ============================================================================
log_header "1. DEVICE VERIFICATION"

SDK=$(getprop ro.build.version.sdk 2>/dev/null || echo "UNKNOWN")
BUILD=$(getprop ro.build.version.incremental 2>/dev/null || echo "UNKNOWN")
DEVICE=$(getprop ro.product.device 2>/dev/null || echo "UNKNOWN")
ABI=$(getprop ro.product.cpu.abi 2>/dev/null || echo "UNKNOWN")
ABI2=$(getprop ro.product.cpu.abi2 2>/dev/null || echo "UNKNOWN")
HARDWARE=$(getprop ro.hardware 2>/dev/null || echo "UNKNOWN")
MANUFACTURER=$(getprop ro.product.manufacturer 2>/dev/null || echo "UNKNOWN")
BRAND=$(getprop ro.product.brand 2>/dev/null || echo "UNKNOWN")

log "ro.build.version.sdk = $SDK"
log "ro.build.version.incremental = $BUILD"
log "ro.product.device = $DEVICE"
log "ro.product.cpu.abi = $ABI"
log "ro.product.cpu.abi2 = $ABI2"
log "ro.hardware = $HARDWARE"
log "ro.product.manufacturer = $MANUFACTURER"
log "ro.product.brand = $BRAND"

# Check against expected values
EXPECTED_SDK="32"
EXPECTED_BUILD="2022.902.48"
EXPECTED_DEVICE="surfaceduo"
EXPECTED_ABI="arm64-v8a"

DEVICE_OK=true

if [ "$SDK" != "$EXPECTED_SDK" ]; then
    log "${RED}FAIL: SDK mismatch (expected $EXPECTED_SDK, got $SDK)${NC}"
    DEVICE_OK=false
else
    log "${GREEN}PASS: SDK matches${NC}"
fi

if [ "$BUILD" != "$EXPECTED_BUILD" ]; then
    log "${RED}FAIL: Build mismatch (expected $EXPECTED_BUILD, got $BUILD)${NC}"
    DEVICE_OK=false
else
    log "${GREEN}PASS: Build matches${NC}"
fi

if [ "$DEVICE" != "$EXPECTED_DEVICE" ]; then
    log "${RED}FAIL: Device mismatch (expected $EXPECTED_DEVICE, got $DEVICE)${NC}"
    DEVICE_OK=false
else
    log "${GREEN}PASS: Device matches${NC}"
fi

if [ "$ABI" != "$EXPECTED_ABI" ]; then
    log "${RED}FAIL: ABI mismatch (expected $EXPECTED_ABI, got $ABI)${NC}"
    DEVICE_OK=false
else
    log "${GREEN}PASS: ABI matches${NC}"
fi

# ============================================================================
# 2. SU CHECK
# ============================================================================
log_header "2. SU CHECK"

if command -v su &> /dev/null; then
    log "${GREEN}PASS: su command found${NC}"
    # Test su access
    if su -c "echo test" 2>/dev/null | grep -q "test"; then
        log "${GREEN}PASS: su access verified${NC}"
    else
        log "${RED}FAIL: su access not working${NC}"
        DEVICE_OK=false
    fi
else
    log "${RED}FAIL: su command not found${NC}"
    DEVICE_OK=false
fi

# ============================================================================
# 3. CAMERA VERIFICATION
# ============================================================================
log_header "3. CAMERA VERIFICATION"

# Get camera count
CAMERA_COUNT=$(dumpsys media.camera 2>/dev/null | grep -c "Camera id =" || echo "0")
log "Number of cameras: $CAMERA_COUNT"

if [ "$CAMERA_COUNT" -eq 0 ]; then
    log "${RED}FAIL: No cameras detected${NC}"
    DEVICE_OK=false
else
    log "${GREEN}PASS: Cameras detected${NC}"
fi

# Get lens facing for each camera
log ""
log "Camera details:"
for i in $(seq 0 $((CAMERA_COUNT - 1))); do
    LENS_FACING=$(dumpsys media.camera 2>/dev/null | grep -A 20 "Camera id = $i" | grep "LENS_FACING" | head -1 || echo "UNKNOWN")
    log "Camera $i: $LENS_FACING"
done

# ============================================================================
# 4. SYMBOL RESOLUTION CHECK
# ============================================================================
log_header "4. SYMBOL RESOLUTION CHECK"

# Check if cameraserver binary exists
CAMERASERVER_BIN="/system/bin/cameraserver"
if [ -f "$CAMERASERVER_BIN" ]; then
    log "${GREEN}PASS: cameraserver binary found${NC}"

    # Try to find the hook symbol
    # android::camera3::Camera3OutputStream::returnBufferCheckedLocked
    # The mangled name is: _ZNK7android7camera318Camera3OutputStream23returnBufferCheckedLockedEPNS0_15Camera3Buffer_tE
    SYMBOL="_ZNK7android7camera318Camera3OutputStream23returnBufferCheckedLockedEPNS0_15Camera3Buffer_tE"

    if nm -D "$CAMERASERVER_BIN" 2>/dev/null | grep -q "$SYMBOL"; then
        log "${GREEN}PASS: Hook symbol found${NC}"
        log "Symbol: $SYMBOL"
    else
        log "${YELLOW}WARNING: Hook symbol not found in export table${NC}"
        log "Trying .gnu_debugdata..."

        # Try to extract from .gnu_debugdata
        if objdump -s -j .gnu_debugdata "$CAMERASERVER_BIN" 2>/dev/null | grep -q "$SYMBOL"; then
            log "${GREEN}PASS: Symbol found in .gnu_debugdata${NC}"
        else
            log "${YELLOW}WARNING: Symbol not found in .gnu_debugdata${NC}"
            log "Trying DWARF info..."

            # Try to extract from DWARF
            if readelf -s "$CAMERASERVER_BIN" 2>/dev/null | grep -q "returnBufferCheckedLocked"; then
                log "${GREEN}PASS: Symbol found in DWARF${NC}"
            else
                log "${RED}FAIL: Could not resolve hook symbol${NC}"
                DEVICE_OK=false
            fi
        fi
    fi
else
    log "${RED}FAIL: cameraserver binary not found at $CAMERASERVER_BIN${NC}"
    DEVICE_OK=false
fi

# ============================================================================
# 5. BUFFER DESCRIPTOR INFO CHECK
# ============================================================================
log_header "5. BUFFER DESCRIPTOR INFO CHECK"

# Try to get a real buffer descriptor from a running camera
# This is device-specific and may require a test app
log "Attempting to capture buffer descriptor info..."

# Create a test buffer and dump its info
TEST_BUFFER="/data/local/tmp/test_buffer.dump"
if dumpsys media.camera 2>/dev/null | grep -A 5 "BufferDescriptor" > "$TEST_BUFFER" 2>/dev/null; then
    log "${GREEN}PASS: Buffer descriptor captured${NC}"
    log "Sample (first 10 lines):"
    head -10 "$TEST_BUFFER" | while read line; do
        log "  $line"
    done
else
    log "${YELLOW}WARNING: Could not capture buffer descriptor${NC}"
    log "This may require a test app to capture real buffer info"
fi

# ============================================================================
# 6. SELINUX AVC CHECK
# ============================================================================
log_header "6. SELINUX AVC CHECK"

# Check for existing AVC denials
AVC_LOG="/data/local/tmp/avc.log"
dmesg 2>/dev/null | grep "avc: denied" > "$AVC_LOG" 2>/dev/null || true

if [ -s "$AVC_LOG" ]; then
    log "Found AVC denials:"
    cat "$AVC_LOG" | while read line; do
        log "  $line"
    done
else
    log "${GREEN}PASS: No AVC denials found (clean SELinux)${NC}"
fi

# ============================================================================
# 7. THERMAL CHECK
# ============================================================================
log_header "7. THERMAL CHECK"

# Get current temperature
TEMP=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo "UNKNOWN")
log "Current temperature: ${TEMP}°C"

if [ "$TEMP" != "UNKNOWN" ] && [ "$TEMP" -lt 42 ]; then
    log "${GREEN}PASS: Temperature within safe range${NC}"
else
    log "${YELLOW}WARNING: Temperature may be high${NC}"
fi

# ============================================================================
# 8. MEMORY CHECK
# ============================================================================
log_header "8. MEMORY CHECK"

# Get memory info
MEM_TOTAL=$(grep MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}' || echo "0")
MEM_FREE=$(grep MemFree /proc/meminfo 2>/dev/null | awk '{print $2}' || echo "0")
MEM_AVAILABLE=$(grep MemAvailable /proc/meminfo 2>/dev/null | awk '{print $2}' || echo "0")

log "Total memory: ${MEM_TOTAL} KB"
log "Free memory: ${MEM_FREE} KB"
log "Available memory: ${MEM_AVAILABLE} KB"

# ============================================================================
# 9. DEVICE-SPECIFIC ENV CHECK
# ============================================================================
log_header "9. DEVICE-SPECIFIC ENV CHECK"

ENV_FILE="/data/local/tmp/devices/surfaceduo.env"
if [ -f "$ENV_FILE" ]; then
    log "${GREEN}PASS: Device env file found${NC}"
    log "Contents:"
    cat "$ENV_FILE" | while read line; do
        log "  $line"
    done
else
    log "${YELLOW}WARNING: Device env file not found at $ENV_FILE${NC}"
fi

# ============================================================================
# 10. BUILD ALLOW LIST CHECK
# ============================================================================
log_header "10. BUILD ALLOW LIST CHECK"

# Check if build is in allow list
if [ "$BUILD" = "2022.902.48" ]; then
    log "${GREEN}PASS: Build is in allow list${NC}"
else
    log "${RED}FAIL: Build not in allow list${NC}"
    DEVICE_OK=false
fi

# ============================================================================
# SUMMARY
# ============================================================================
log_header "SUMMARY"

if [ "$DEVICE_OK" = true ]; then
    log "${GREEN}PREFLIGHT CHECK PASSED${NC}"
    log "Device is ready for camswap installation"
    PREFLIGHT_RESULT="PASS"
else
    log "${RED}PREFLIGHT CHECK FAILED${NC}"
    log "Device does not meet requirements"
    PREFLIGHT_RESULT="FAIL"
fi

log ""
log "Device info saved to: $LOGFILE"

# Create preflight report
cat > /data/local/tmp/docs/PREFLIGHT.md << EOF
# Preflight Report

**Date:** $(date -Iseconds)
**Result:** $PREFLIGHT_RESULT

## Device Information

| Property | Value | Expected | Status |
|----------|-------|----------|--------|
| SDK | $SDK | $EXPECTED_SDK | $([ "$SDK" = "$EXPECTED_SDK" ] && echo "PASS" || echo "FAIL") |
| Build | $BUILD | $EXPECTED_BUILD | $([ "$BUILD" = "$EXPECTED_BUILD" ] && echo "PASS" || echo "FAIL") |
| Device | $DEVICE | $EXPECTED_DEVICE | $([ "$DEVICE" = "$EXPECTED_DEVICE" ] && echo "PASS" || echo "FAIL") |
| ABI | $ABI | $EXPECTED_ABI | $([ "$ABI" = "$EXPECTED_ABI" ] && echo "PASS" || echo "FAIL") |

## Camera Information

- Number of cameras: $CAMERA_COUNT

## Symbol Resolution

- Hook symbol: $SYMBOL
- Status: $([ "$DEVICE_OK" = true ] && echo "RESOLVED" || echo "NOT RESOLVED")

## Thermal

- Current temperature: ${TEMP}°C

## Memory

- Total: ${MEM_TOTAL} KB
- Free: ${MEM_FREE} KB
- Available: ${MEM_AVAILABLE} KB

## SELinux

- AVC denials: $(wc -l < "$AVC_LOG" 2>/dev/null || echo "0")

## Conclusion

$([ "$DEVICE_OK" = true ] && echo "✅ Device is ready for camswap installation" || echo "❌ Device does not meet requirements")

EOF

# Exit with appropriate code
if [ "$DEVICE_OK" = true ]; then
    exit 0
else
    exit 3
fi
