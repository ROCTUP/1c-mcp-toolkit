#!/usr/bin/env bash
set -euo pipefail
cmake -S /src -B /src/build_linux -DCMAKE_BUILD_TYPE=Release -DSCREENCAPTURE_TESTS=ON
cmake --build /src/build_linux --parallel "$(nproc)"
(cd /src/build_linux && ctest --output-on-failure)
symbols=$(nm -D --defined-only /src/build_linux/ScreenCapture.so | awk '{print $3}')
for name in GetClassNames GetClassObject DestroyObject SetPlatformCapabilities; do
    grep -Fxq "$name" <<< "$symbols" || { echo "Missing export: $name" >&2; exit 1; }
done
ldd -r /src/build_linux/ScreenCapture.so | tee /src/build_linux/dependencies.txt
if grep -E 'not found|undefined symbol' /src/build_linux/dependencies.txt; then exit 1; fi
file /src/build_linux/ScreenCapture.so 2>/dev/null || true
sha256sum /src/build_linux/ScreenCapture.so
