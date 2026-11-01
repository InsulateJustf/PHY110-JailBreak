#!/bin/bash
# Shared environment for the PHY110 / 6.1.141 port.
export REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export WORK="$REPO/work"
export NDK="${NDK:-/opt/homebrew/share/android-ndk}"
export CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android31-clang"
export PATH="$WORK/tools/bin:$PATH"
export UV_TOOL_DIR="$WORK/tools/uv"
export UV_TOOL_BIN_DIR="$WORK/tools/bin"
export ADB_SERIAL="${ADB_SERIAL:-c744ea95}"
adb() { command adb -s "$ADB_SERIAL" "$@"; }
