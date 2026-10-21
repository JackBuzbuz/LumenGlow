#!/bin/bash
# Builds LumenGlow.plugin (universal: Apple Silicon + Intel).
# Usage: ./build_mac.sh /path/to/AfterEffectsSDK
set -euo pipefail

SDK="${1:-}"
if [[ -z "$SDK" || ! -f "$SDK/Examples/Headers/AE_Effect.h" ]]; then
    echo "Usage: $0 /path/to/AfterEffectsSDK   (the folder that contains Examples/)" >&2
    exit 1
fi
command -v cmake >/dev/null || { echo "CMake not found. Install it with: brew install cmake" >&2; exit 1; }
# Use Xcode even if xcode-select points at the Command Line Tools.
if ! xcodebuild -version >/dev/null 2>&1 && [[ -d /Applications/Xcode.app ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
xcodebuild -version >/dev/null 2>&1 || { echo "Xcode not found. Install Xcode from the App Store." >&2; exit 1; }

cd "$(dirname "$0")"
cmake -S . -B build -G Xcode -DAE_SDK_DIR="$SDK"
cmake --build build --config Release

echo
echo "Built: $(pwd)/build/Release/LumenGlow.plugin"
