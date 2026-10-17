#!/usr/bin/env bash
# =====================================================================
# LebensSpur build betigi — tum ciktilar esp_bin/ altinda kalir.
#
#   ./esp_bin/build.sh                 -> build
#   ./esp_bin/build.sh flash monitor   -> idf.py argumanlari aynen gecer
#   ./esp_bin/build.sh fullclean
#
# Ortam: IDF_DIR ile ESP-IDF yolu degistirilebilir.
# Kaynaklar: ALTE/HW (firmware) + ALTE/GUI (web arayuzu, firmware'e gomulur)
# =====================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$ROOT/esp_bin/HW"
IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf-v5.5.2}"

if [ ! -f "$IDF_DIR/export.sh" ]; then
  echo "HATA: ESP-IDF bulunamadi: $IDF_DIR (IDF_DIR ile belirt)" >&2
  exit 1
fi

# ESP-IDF python ortami: export.sh PATH'teki python3 surumune gore ortam arar
# (orn. py3.15) ve o kurulu degilse hata verir. Kurulu bir idf5.5 ortami varsa onu kullan.
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

cd "$PROJ"
ARGS=("$@")
[ ${#ARGS[@]} -eq 0 ] && ARGS=(build)

idf.py "${ARGS[@]}"

# Basarili build sonrasi firmware'i surum adiyla kopyala (OTA icin)
if [[ " ${ARGS[*]} " == *" build "* ]] && [ -f "$PROJ/build/lebensspur.bin" ]; then
  VER=$(sed -nE 's/.*set\(PROJECT_VER "([^"]+)"\).*/\1/p' "$ROOT/ALTE/HW/CMakeLists.txt")
  OUT="$ROOT/esp_bin/firmware"
  mkdir -p "$OUT"
  cp "$PROJ/build/lebensspur.bin" "$OUT/lebensspur_v${VER}.bin"
  cp "$PROJ/build/lebensspur.bin" "$OUT/lebensspur_latest.bin"
  echo
  echo "Firmware: $OUT/lebensspur_v${VER}.bin"
fi
