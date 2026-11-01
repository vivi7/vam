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
    else
        echo -e "${RED}Error: NDK not found. Please set ANDROID_NDK_HOME or NDK_ROOT${NC}"
        exit 1
    fi
fi

echo "NDK root: $NDK_ROOT"

# Create module directory structure
echo ""
echo "=== Creating module structure ==="
cd "$PROJECT_DIR"

# Clean previous build
rm -rf module/system
mkdir -p module/system/bin
mkdir -p module/system/etc/camswap/identities

# Copy device config
echo "Copying device config..."
cp -f devices/surfaceduo.env module/system/etc/camswap/device.env

# Copy binaries (placeholder - will be replaced after build)
echo "Creating placeholder binaries..."
cat > module/system/bin/camswap_payload << 'EOF'
#!/system/bin/sh
# Camswap payload - placeholder
exec /data/local/tmp/camswap_payload.so "$@"
EOF
chmod 755 module/system/bin/camswap_payload

cat > module/system/bin/camswap_daemon << 'EOF'
#!/system/bin/sh
# Camswap daemon - placeholder
exec /data/local/tmp/camswap_daemon "$@"
EOF
chmod 755 module/system/bin/camswap_daemon

# Create Magisk module zip
MODULE_NAME="camswap-v1.0.0.zip"
echo ""
echo "=== Creating Magisk module ==="
cd module
zip -r "../$MODULE_NAME" . -x "*.DS_Store" -x "__MACOSX/*" -x "libs/*"
cd ..

echo -e "${GREEN}Module created: $PROJECT_DIR/$MODULE_NAME${NC}"

# Print module contents
echo ""
echo "=== Module contents ==="
unzip -l "$MODULE_NAME" | head -30

echo ""
echo "=== Build complete ==="
echo ""
echo "To install on device:"
echo "  1. Copy $MODULE_NAME to device"
echo "  2. Open Magisk app"
echo "  3. Install from storage"
echo "  4. Reboot device"
echo ""
echo "To verify installation:"
echo "  adb shell 'su -c /data/local/tmp/preflight.sh'"
