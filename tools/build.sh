#!/bin/bash
# build.sh - Build script for camswap

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Camswap Build Script ==="
echo "Project directory: $PROJECT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Check for NDK
if [ -z "$NDK_ROOT" ]; then
    echo -e "${YELLOW}NDK_ROOT not set, trying to find NDK...${NC}"
    if [ -d "$ANDROID_NDK_HOME" ]; then
        NDK_ROOT="$ANDROID_NDK_HOME"
        echo "Found NDK at $NDK_ROOT"
    elif [ -d "/usr/local/android-ndk" ]; then
        NDK_ROOT="/usr/local/android-ndk"
        echo "Found NDK at $NDK_ROOT"
    elif [ -d "/opt/android-ndk" ]; then
        NDK_ROOT="/opt/android-ndk"
        echo "Found NDK at $NDK_ROOT"
    elif [ -d "/Applications/SDKs/android/ndk" ]; then
        LATEST_NDK="$(ls -1 "/Applications/SDKs/android/ndk" 2>/dev/null | sort -V | tail -n 1)"
        NDK_ROOT="/Applications/SDKs/android/ndk/$LATEST_NDK"
        echo "Found NDK at $NDK_ROOT"
    else
        echo -e "${RED}Error: NDK not found. Please set ANDROID_NDK_HOME or NDK_ROOT${NC}"
        exit 1
    fi
fi

echo "NDK root: $NDK_ROOT"
echo "NDK version: $(cat $NDK_ROOT/RELEASE.txt 2>/dev/null || echo 'unknown')"

# Build payload
echo ""
echo "=== Building payload ==="
cd "$PROJECT_DIR/payload"
if [ -f "Android.mk" ]; then
    echo "Building payload with ndk-build..."
    $NDK_ROOT/ndk-build \
        NDK_PROJECT_PATH=. \
        APP_BUILD_SCRIPT=Android.mk \
        APP_PLATFORM=android-31 \
        APP_ABI=arm64-v8a \
        APP_OPTIM=release \
        V=1
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Payload built successfully${NC}"
    else
        echo -e "${RED}Payload build failed${NC}"
        exit 1
    fi
else
    echo -e "${YELLOW}No Android.mk found in payload directory${NC}"
fi

# Build daemon
echo ""
echo "=== Building daemon ==="
cd "$PROJECT_DIR/daemon"
if [ -f "Android.mk" ]; then
    echo "Building daemon with ndk-build..."
    $NDK_ROOT/ndk-build \
        NDK_PROJECT_PATH=. \
        APP_BUILD_SCRIPT=Android.mk \
        APP_PLATFORM=android-31 \
        APP_ABI=arm64-v8a \
        APP_OPTIM=release \
        V=1
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Daemon built successfully${NC}"
    else
        echo -e "${RED}Daemon build failed${NC}"
        exit 1
    fi
else
    echo -e "${YELLOW}No Android.mk found in daemon directory${NC}"
fi

# Build app
echo ""
echo "=== Building app ==="
cd "$PROJECT_DIR/app"
if [ -f "build.gradle.kts" ]; then
    echo "Building app with Gradle..."
    if command -v ./gradlew &> /dev/null; then
        ./gradlew assembleRelease
    elif command -v gradle &> /dev/null; then
        gradle assembleRelease
    else
        echo -e "${YELLOW}Gradle not found, skipping app build${NC}"
    fi
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}App built successfully${NC}"
    else
        echo -e "${RED}App build failed${NC}"
        exit 1
    fi
else
    echo -e "${YELLOW}No build.gradle.kts found in app directory${NC}"
fi

# Create Magisk module
echo ""
echo "=== Creating Magisk module ==="
cd "$PROJECT_DIR"

# Copy binaries to module
mkdir -p module/system/bin
cp -f payload/libs/arm64-v8a/libcamswap_payload.so module/system/bin/camswap_payload 2>/dev/null || true
cp -f daemon/libs/arm64-v8a/camswap_daemon module/system/bin/camswap_daemon 2>/dev/null || true

# Create module zip
MODULE_NAME="camswap-v1.0.0.zip"
cd module
zip -r "../$MODULE_NAME" . -x "*.DS_Store" -x "__MACOSX/*"
cd ..

echo -e "${GREEN}Module created: $PROJECT_DIR/$MODULE_NAME${NC}"

echo ""
echo "=== Build complete ==="
