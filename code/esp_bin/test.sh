#!/usr/bin/env bash
# =====================================================================
# LebensSpur — masaustu (host) birim testleri.
#   ./code/esp_bin/test.sh            -> derle + ctest
#   ./code/esp_bin/test.sh -DSAN=ON   -> AddressSanitizer/UBSan ile
# Cikti: code/esp_bin/test-build/ (git'e girmez)
# =====================================================================
set -euo pipefail
BIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # code/esp_bin
PROJ="$(cd "$BIN_DIR/.." && pwd)"                          # code
IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf-v5.5.2}"
# Bu Mac'te CLT'nin varsayilan SDK'si (27.2) Xcode ld ile baglanmiyor; Xcode SDK'sini sec
if command -v xcrun >/dev/null 2>&1; then
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
fi
BUILD="$BIN_DIR/test-build"
SRC="$PROJ/test/host"
# Kaynak klasoru tasindiysa eski onbellek reddedilir: --fresh ile yeniden yapilandir
FRESH=()
if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$BUILD/CMakeCache.txt"; then
  FRESH=(--fresh)
fi
cmake ${FRESH[@]+"${FRESH[@]}"} -S "$SRC" -B "$BUILD" -G Ninja -DIDF_PATH="$IDF_DIR" "$@" >/dev/null
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
