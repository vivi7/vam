#!/bin/bash
# build_module.sh - Build script for camswap Magisk module

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Camswap Module Builder ==="
echo "Project directory: $PROJECT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Resolve NDK if needed to compile binaries
if [ -z "$NDK_ROOT" ]; then
    if [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ]; then
        NDK_ROOT="$ANDROID_NDK_HOME"
    elif [ -d "/usr/local/android-ndk" ]; then
        NDK_ROOT="/usr/local/android-ndk"
    elif [ -d "/opt/android-ndk" ]; then
        NDK_ROOT="/opt/android-ndk"
    elif [ -d "/Applications/SDKs/android/ndk" ]; then
        LATEST_NDK="$(ls -1 "/Applications/SDKs/android/ndk" 2>/dev/null | sort -V | tail -n 1)"
        if [ -n "$LATEST_NDK" ]; then
            NDK_ROOT="/Applications/SDKs/android/ndk/$LATEST_NDK"
        fi
    elif [ -d "$HOME/Library/Android/sdk/ndk" ]; then
        LATEST_NDK="$(ls -1 "$HOME/Library/Android/sdk/ndk" 2>/dev/null | sort -V | tail -n 1)"
        if [ -n "$LATEST_NDK" ]; then
            NDK_ROOT="$HOME/Library/Android/sdk/ndk/$LATEST_NDK"
        fi
    fi
fi

cd "$PROJECT_DIR"

# Build native binaries if NDK is available
if [ -n "$NDK_ROOT" ] && [ -x "$NDK_ROOT/ndk-build" ]; then
    echo "Found NDK at: $NDK_ROOT"
    echo "=== Building payload & daemon with ndk-build ==="
    (cd "$PROJECT_DIR/payload" && "$NDK_ROOT/ndk-build" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_PLATFORM=android-31 APP_ABI=arm64-v8a APP_OPTIM=release)
    (cd "$PROJECT_DIR/daemon" && "$NDK_ROOT/ndk-build" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_PLATFORM=android-31 APP_ABI=arm64-v8a APP_OPTIM=release)
else
    echo -e "${YELLOW}NDK not found on PATH; packaging prebuilt binaries from daemon/libs and payload/libs${NC}"
fi

echo ""
echo "=== Staging Magisk module structure ==="
mkdir -p module/system/bin
mkdir -p module/system/lib64
mkdir -p module/system/etc/camswap/models
mkdir -p module/system/etc/camswap/identities

# Copy device config
echo "Copying device config..."
cp -f devices/surfaceduo.env module/system/etc/camswap/device.env

# Package real compiled daemon and payload binaries (never overwrite with shell wrappers)
if [ -f "daemon/libs/arm64-v8a/camswap_daemon" ]; then
    cp -f "daemon/libs/arm64-v8a/camswap_daemon" "module/system/bin/camswap_daemon"
elif [ ! -f "module/system/bin/camswap_daemon" ]; then
    echo -e "${RED}Error: daemon binary not found at daemon/libs/arm64-v8a/camswap_daemon${NC}"
    exit 1
fi
chmod 755 "module/system/bin/camswap_daemon"

if [ -f "payload/libs/arm64-v8a/libcamswap_payload.so" ]; then
    cp -f "payload/libs/arm64-v8a/libcamswap_payload.so" "module/system/bin/camswap_payload"
    cp -f "payload/libs/arm64-v8a/libcamswap_payload.so" "module/system/lib64/libcamswap_payload.so"
fi
[ -f "module/system/bin/camswap_payload" ] && chmod 755 "module/system/bin/camswap_payload"

# Package libonnxruntime.so and any companion .so if staged in daemon/prebuilt or module/system/lib64
if [ -d "daemon/prebuilt/arm64-v8a" ]; then
    cp -f "daemon/prebuilt/arm64-v8a/"*.so "module/system/lib64/" 2>/dev/null || true
fi

# Package models & emap if present in models/ or refs/
if [ -d "models" ]; then
    cp -f "models/"* "module/system/etc/camswap/models/" 2>/dev/null || true
fi
if [ -f "refs/android-face-fusion/app/src/main/assets/emap.bin" ] && [ ! -f "module/system/etc/camswap/models/emap.bin" ]; then
    cp -f "refs/android-face-fusion/app/src/main/assets/emap.bin" "module/system/etc/camswap/models/emap.bin"
fi
if [ -f "refs/inswapper-512-live/identity_default.bin" ] && [ ! -f "module/system/etc/camswap/identities/default.bin" ]; then
    cp -f "refs/inswapper-512-live/identity_default.bin" "module/system/etc/camswap/identities/default.bin"
fi

# Create Magisk module zip
MODULE_NAME="camswap-v1.0.0.zip"
echo ""
echo "=== Creating Magisk module archive ==="
rm -f "$MODULE_NAME"
(
    cd module
    zip -r "../$MODULE_NAME" . -x "*.DS_Store" -x "__MACOSX/*"
)

echo -e "${GREEN}Module created: $PROJECT_DIR/$MODULE_NAME${NC}"
echo ""
echo "=== Module contents ==="
unzip -l "$MODULE_NAME"
