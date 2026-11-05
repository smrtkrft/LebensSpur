#!/usr/bin/env bash
# =====================================================================
# LebensSpur build betigi — tum ciktilar esp_bin/ altinda kalir.
#
#   ./esp_bin/build.sh                       -> 2611 build
#   ./esp_bin/build.sh -p PORT flash monitor -> idf.py argumanlari aynen gecer
#   ./esp_bin/build.sh test                  -> masaustu birim testleri (esp_bin/test.sh)
#   ./esp_bin/build.sh --alte [args]         -> eski ALTE agaci (esp_bin/HW)
#
# Ortam: IDF_DIR ile ESP-IDF yolu degistirilebilir.
# =====================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" = "test" ]; then
  shift
  exec "$ROOT/esp_bin/test.sh" "$@"
fi

TREE=2611
if [ "${1:-}" = "--alte" ]; then
  TREE=alte
  shift
fi

if [ "$TREE" = "alte" ]; then
  PROJ="$ROOT/esp_bin/HW"
  VER_SRC="$ROOT/ALTE/HW/CMakeLists.txt"
  FW_PREFIX="alte_lebensspur"
  SIZE_LIMIT=0
else
  PROJ="$ROOT/esp_bin/2611"
  VER_SRC="$ROOT/2611/HW/CMakeLists.txt"
  FW_PREFIX="lebensspur"
  SIZE_LIMIT=$((1536 * 1024))   # 1,5 MiB imaj siniri (04-plan-son.md kuresel kisitlar)
fi

IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf-v5.5.2}"
if [ ! -f "$IDF_DIR/export.sh" ]; then
  echo "HATA: ESP-IDF bulunamadi: $IDF_DIR (IDF_DIR ile belirt)" >&2
  exit 1
fi

# ESP-IDF python ortami: kurulu bir idf5.5 ortami varsa onu kullan
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

idf.py -C "$PROJ" "${ARGS[@]}"

# Basarili build sonrasi: boyut denetimi + surum adiyla kopya (OTA icin)
BIN="$PROJ/build/lebensspur.bin"
if [[ " ${ARGS[*]} " == *" build "* || " ${ARGS[*]} " == *" flash "* ]] && [ -f "$BIN" ]; then
  VER=$(sed -nE 's/.*set\(PROJECT_VER "([^"]+)"\).*/\1/p' "$VER_SRC")
  SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
  echo
  echo "Imaj: $SIZE bayt ($((SIZE / 1024)) KiB), surum $VER"
  if [ "$SIZE_LIMIT" -gt 0 ] && [ "$SIZE" -gt "$SIZE_LIMIT" ]; then
    echo "HATA: imaj $SIZE bayt > sinir $SIZE_LIMIT bayt (1,5 MiB)" >&2
    exit 2
  fi
  OUT="$ROOT/esp_bin/firmware"
  mkdir -p "$OUT"
  cp "$BIN" "$OUT/${FW_PREFIX}_v${VER}.bin"
  cp "$BIN" "$OUT/${FW_PREFIX}_latest.bin"
  [ -f "$PROJ/build/lebensspur.elf" ] && cp "$PROJ/build/lebensspur.elf" "$OUT/${FW_PREFIX}_v${VER}.elf"
  echo "Firmware: $OUT/${FW_PREFIX}_v${VER}.bin"
fi
