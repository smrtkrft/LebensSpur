#!/usr/bin/env bash
# =====================================================================
# LebensSpur derleme betiği — tüm çıktılar code/esp_bin/ altında kalır.
#
#   ./code/esp_bin/build.sh                       -> derle
#   ./code/esp_bin/build.sh -p PORT flash monitor -> idf.py argümanları aynen geçer
#   ./code/esp_bin/build.sh test                  -> masaüstü birim testleri (test.sh)
#
# Ortam: IDF_DIR ile ESP-IDF yolu değiştirilebilir.
# =====================================================================
set -euo pipefail

BIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # code/esp_bin
PROJ="$(cd "$BIN_DIR/.." && pwd)"                          # code

if [ "${1:-}" = "test" ]; then
  shift
  exec "$BIN_DIR/test.sh" "$@"
fi

SIZE_LIMIT=$((1536 * 1024))   # 1,5 MiB imaj sınırı (docs/04-plan-son.md küresel kısıtlar)
FW_PREFIX="lebensspur"
BUILD="$BIN_DIR/build"

IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf-v5.5.2}"
if [ ! -f "$IDF_DIR/export.sh" ]; then
  echo "HATA: ESP-IDF bulunamadi: $IDF_DIR (IDF_DIR ile belirt)" >&2
  exit 1
fi

# ESP-IDF python ortamı: kurulu bir idf5.5 ortamı varsa onu kullan
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
  for d in "$HOME"/.espressif/python_env/idf5.5_py*_env; do
    if [ -x "$d/bin/python" ]; then
      export IDF_PYTHON_ENV_PATH="$d"
      break
    fi
  done
fi

# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/dev/null

ARGS=("$@")
[ ${#ARGS[@]} -eq 0 ] && ARGS=(build)

idf.py -C "$PROJ" -B "$BUILD" "${ARGS[@]}"

# Başarılı derleme sonrası: boyut denetimi + sürüm adıyla kopya (OTA yayını için)
BIN="$BUILD/lebensspur.bin"
if [[ " ${ARGS[*]} " == *" build "* || " ${ARGS[*]} " == *" flash "* ]] && [ -f "$BIN" ]; then
  VER=$(sed -nE 's/.*set\(PROJECT_VER "([^"]+)"\).*/\1/p' "$PROJ/CMakeLists.txt")
  SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
  echo
  echo "Imaj: $SIZE bayt ($((SIZE / 1024)) KiB), surum $VER"
  if [ "$SIZE" -gt "$SIZE_LIMIT" ]; then
    echo "HATA: imaj $SIZE bayt > sinir $SIZE_LIMIT bayt (1,5 MiB)" >&2
    exit 2
  fi
  OUT="$BIN_DIR/firmware"
  mkdir -p "$OUT"
  cp "$BIN" "$OUT/${FW_PREFIX}_v${VER}.bin"
  cp "$BIN" "$OUT/${FW_PREFIX}_latest.bin"
  [ -f "$BUILD/lebensspur.elf" ] && cp "$BUILD/lebensspur.elf" "$OUT/${FW_PREFIX}_v${VER}.elf"
  echo "Firmware: $OUT/${FW_PREFIX}_v${VER}.bin"
fi
