#!/usr/bin/env bash
# =====================================================================
# LebensSpur 2611 — masaustu (host) birim testleri.
#   ./esp_bin/test.sh            -> derle + ctest
#   ./esp_bin/test.sh -DSAN=ON   -> AddressSanitizer/UBSan ile
# Cikti: esp_bin/test-build/ (git'e girmez)
# =====================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf-v5.5.2}"
# Bu Mac'te CLT'nin varsayilan SDK'si (27.2) Xcode ld ile baglanmiyor; Xcode SDK'sini sec
if command -v xcrun >/dev/null 2>&1; then
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
fi
BUILD="$ROOT/esp_bin/test-build"
cmake -S "$ROOT/2611/HW/test/host" -B "$BUILD" -G Ninja -DIDF_PATH="$IDF_DIR" "$@" >/dev/null
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
