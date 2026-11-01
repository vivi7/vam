#!/bin/bash
# harness.sh - Test harness for camswap

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Default test parameters
TEST_DURATION=600  # 10 minutes
APP_LIST="whatsapp browser camera"
FPS_TARGET=20
FPS_FLOOR=15
TEMP_THRESHOLD=42

echo "=== Camswap Test Harness ==="
echo "Test duration: ${TEST_DURATION}s"
echo "Apps to test: $APP_LIST"
echo "FPS target: $FPS_TARGET"
echo "FPS floor: $FPS_FLOOR"
echo "Temp threshold: $TEMP_THRESHOLD°C"

# Check if device is connected
if ! command -v adb &> /dev/null; then
    echo -e "${RED}Error: adb not found${NC}"
    exit 1
fi

if ! adb devices | grep -q "device$"; then
    echo -e "${RED}Error: No device connected${NC}"
    exit 1
fi

DEVICE_SERIAL=$(adb devices | grep "device$" | head -1 | awk '{print $1}')
echo "Device: $DEVICE_SERIAL"

# Get device info
DEVICE_NAME=$(adb shell getprop ro.product.device)
SDK_VERSION=$(adb shell getprop ro.build.version.sdk)
BUILD_NUMBER=$(adb shell getprop ro.build.version.incremental)

echo "Device name: $DEVICE_NAME"
echo "SDK version: $SDK_VERSION"
echo "Build number: $BUILD_NUMBER"

# Check device compatibility
if [ "$DEVICE_NAME" != "surfaceduo" ]; then
    echo -e "${RED}Error: Device is not Surface Duo${NC}"
    exit 1
fi

if [ "$SDK_VERSION" != "32" ]; then
    echo -e "${RED}Error: SDK version is not 32${NC}"
    exit 1
fi

# Push module to device
echo ""
echo "=== Pushing module to device ==="
adb root
adb wait-for-device

# Stop any existing camswap
echo "Stopping existing camswap..."
adb shell "su -c '/data/local/tmp/payload_stop' 2>/dev/null" || true
adb shell "stop cameraserver 2>/dev/null" || true
sleep 2

# Push binaries
echo "Pushing binaries..."
adb push "$PROJECT_DIR/payload/libs/arm64-v8a/libcamswap_payload.so" /data/local/tmp/camswap_payload.so
adb push "$PROJECT_DIR/daemon/libs/arm64-v8a/camswap_daemon" /data/local/tmp/camswap_daemon
adb push "$PROJECT_DIR/tools/preflight.sh" /data/local/tmp/preflight.sh
adb shell "chmod 755 /data/local/tmp/camswap_payload.so"
adb shell "chmod 755 /data/local/tmp/camswap_daemon"
adb shell "chmod 755 /data/local/tmp/preflight.sh"

# Start cameraserver
echo "Starting cameraserver..."
adb shell "start cameraserver"
sleep 2

# Run tests
echo ""
echo "=== Running tests ==="

# Create test report
REPORT_DIR="$PROJECT_DIR/docs/reports/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$REPORT_DIR"

# Test each app
for APP in $APP_LIST; do
    echo ""
    echo "--- Testing $APP ---"

    # Start app
    echo "Starting $APP..."
    case $APP in
        whatsapp)
            adb shell "am start -n com.whatsapp/.Main"
            ;;
        browser)
            adb shell "am start -n com.android.browser/.BrowserActivity"
            ;;
        camera)
            adb shell "am start -n com.android.camera/.Camera"
            ;;
        *)
            echo "Unknown app: $APP"
            continue
            ;;
    esac

    sleep 2

    # Start logcat
    LOG_FILE="$REPORT_DIR/${APP}_logcat.txt"
    adb logcat -b main -b system -b crash > "$LOG_FILE" &
    LOGCAT_PID=$!

    # Monitor for test duration
    START_TIME=$(date +%s)
    END_TIME=$((START_TIME + TEST_DURATION))

    while [ $(date +%s) -lt $END_TIME ]; do
        # Check FPS
        FPS=$(adb shell "dumpsys media.camera 2>/dev/null | grep -o 'fps=[0-9]*' | head -1 | cut -d= -f2" || echo "0")

        # Check temperature
        TEMP=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo "0")

        # Check for crashes
        if adb logcat -d | grep -q "FATAL"; then
            echo -e "${RED}Crash detected!${NC}"
            break
        fi

        echo "FPS: $FPS, Temp: ${TEMP}°C"

        # Check FPS threshold
        if [ "$FPS" -lt "$FPS_FLOOR" ] && [ "$FPS" -gt 0 ]; then
            echo -e "${YELLOW}FPS below floor: $FPS < $FPS_FLOOR${NC}"
        fi

        # Check temperature threshold
        if [ "$TEMP" -gt "$TEMP_THRESHOLD" ]; then
            echo -e "${YELLOW}Temperature above threshold: ${TEMP}°C > ${TEMP_THRESHOLD}°C${NC}"
        fi

        sleep 5
    done

    # Stop logcat
    kill $LOGCAT_PID 2>/dev/null || true

    # Stop app
    echo "Stopping $APP..."
    adb shell "am force-stop $APP"

    sleep 2
done

# Generate report
echo ""
echo "=== Generating report ==="

cat > "$REPORT_DIR/report.md" << EOF
# Camswap Test Report

**Date:** $(date -Iseconds)
**Device:** $DEVICE_NAME
**SDK:** $SDK_VERSION
**Build:** $BUILD_NUMBER

## Test Parameters

- Duration: ${TEST_DURATION}s per app
- Apps: $APP_LIST
- FPS target: $FPS_TARGET
- FPS floor: $FPS_FLOOR
- Temp threshold: ${TEMP_THRESHOLD}°C

## Results

EOF

for APP in $APP_LIST; do
    echo "## $APP" >> "$REPORT_DIR/report.md"
    echo "" >> "$REPORT_DIR/report.md"
    echo "Log file: ${APP}_logcat.txt" >> "$REPORT_DIR/report.md"
    echo "" >> "$REPORT_DIR/report.md"
done

echo -e "${GREEN}Report generated: $REPORT_DIR/report.md${NC}"

echo ""
echo "=== Tests complete ==="
