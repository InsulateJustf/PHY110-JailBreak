#!/bin/bash
# Install a project-local JDK + Android SDK (no sudo) and build the APK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/work/tools"
mkdir -p "$W"

if [ ! -x "$W/jdk21/Contents/Home/bin/java" ]; then
  echo "== JDK 21 =="
  curl -L --retry 3 -o "$W/jdk21.tar.gz" \
    "https://api.adoptium.net/v3/binary/latest/21/ga/mac/aarch64/jdk/hotspot/normal/eclipse"
  rm -rf "$W/jdk21"; mkdir -p "$W/jdk21"
  tar -xzf "$W/jdk21.tar.gz" -C "$W/jdk21" --strip-components=1
fi
export JAVA_HOME="$W/jdk21/Contents/Home"
export PATH="$JAVA_HOME/bin:$PATH"
"$JAVA_HOME/bin/java" -version 2>&1 | head -1

SDK="$W/android-sdk"
if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
  echo "== cmdline-tools =="
  mkdir -p "$SDK/cmdline-tools"
  curl -L --retry 3 -o "$W/cmdline-tools.zip" \
    "https://dl.google.com/android/repository/commandlinetools-mac-11076708_latest.zip"
  rm -rf "$W/cmdline-tools-x"
  unzip -q -o "$W/cmdline-tools.zip" -d "$W/cmdline-tools-x"
  rm -rf "$SDK/cmdline-tools/latest"
  mv "$W/cmdline-tools-x/cmdline-tools" "$SDK/cmdline-tools/latest"
fi

echo "== sdk packages =="
yes | "$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" --licenses >/dev/null || true
"$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" \
  "platforms;android-37.0" "build-tools;37.0.0" "platform-tools" 2>&1 | tail -5 || \
"$SDK/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$SDK" \
  "platforms;android-37.0" "platform-tools" 2>&1 | tail -5

echo "sdk.dir=$SDK" > "$ROOT/app/local.properties"

echo "== gradle build =="
cd "$ROOT/app"
JAVA_HOME="$JAVA_HOME" ./gradlew :app:assembleDebug --no-daemon 2>&1 | tail -25
ls -l app/build/outputs/apk/debug/ 2>/dev/null || true
