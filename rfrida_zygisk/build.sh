#!/bin/bash
set -euo pipefail
# Build rfrida-zygisk Zygisk module for arm64 Android
# Requires Android NDK, uses CMake + ninja

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-$HOME/Library/Android/sdk/ndk/27.0.12077973}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
API_LEVEL=26
TARGET=aarch64-linux-android
BUILD_DIR="$SCRIPT_DIR/build"

echo "NDK: $NDK"
echo "Target: $TARGET (API $API_LEVEL)"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cmake -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$API_LEVEL" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    "$SCRIPT_DIR/module/src/main/cpp"

cmake --build "$BUILD_DIR" -j$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "=== Build complete ==="
ls -la "$BUILD_DIR/librfrida_zygisk.so" 2>/dev/null && echo "Module built!" || echo "FAILED"
