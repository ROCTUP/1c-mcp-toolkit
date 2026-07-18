#!/usr/bin/env bash
# Build a 1C Native API component for macOS as a universal (x86_64 + arm64) .dylib.
#
# Why universal: the 1C:Enterprise client on macOS ships as an x86_64 binary and
# runs under Rosetta 2 on Apple Silicon. An arm64-only library cannot be loaded
# into that x86_64 process and fails with "Тип не определен (AddIn.*.*)".
# A universal binary works both natively and under Rosetta.
#
# Requirements: CMake 3.16+, Apple clang (Xcode Command Line Tools).
# No Docker, no Python — just the native toolchain.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME="$(basename "$SCRIPT_DIR")"
cd "$SCRIPT_DIR"

rm -rf build_macos
cmake -B build_macos -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build build_macos -j"$(sysctl -n hw.ncpu)"

DYLIB="build_macos/${NAME}.dylib"

echo '--- Verify architectures ---'
lipo -archs "$DYLIB"
lipo -archs "$DYLIB" | grep -q x86_64 || { echo '[ERROR] x86_64 slice missing'; exit 1; }
lipo -archs "$DYLIB" | grep -q arm64  || { echo '[ERROR] arm64 slice missing';  exit 1; }

echo '--- Verify exports ---'
EXPORTS=$(nm -gU "$DYLIB" | grep -cE 'GetClassNames|GetClassObject|DestroyObject|SetPlatformCapabilities' || true)
echo "Exports found: ${EXPORTS}/4"
[ "$EXPORTS" -eq 4 ] || { echo '[ERROR] Not all 4 required 1C Native API exports found'; exit 1; }

echo '--- Ad-hoc code signing (avoids hardened-runtime load rejection) ---'
codesign --force --sign - "$DYLIB"

echo "Done: ${SCRIPT_DIR}/${DYLIB}"
