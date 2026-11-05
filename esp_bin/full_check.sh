#!/usr/bin/env bash
# =====================================================================
# Tam kapsamlı denetim turu (her düzeltmeden sonra baştan çalıştırılır):
#   1) masaüstü birim testleri   2) derleme (-Werror) + flash   3) API testi
#   4) arayüz ekran/JS denetimi  5) arayüz ↔ cihaz gidiş-dönüş testi
#   6) güvenlik (parolasız ↔ parolalı karşılaştırmalı)
#
#   ./esp_bin/full_check.sh IP PORT OUT_DIR
# Çıkış kodu 0 = tüm katmanlar hatasız. Özet OUT_DIR/ozet.txt
# =====================================================================
set -uo pipefail
IP="$1"; PORT="$2"; OUT="$3"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY=/Users/seu/.espressif/python_env/idf5.5_py3.9_env/bin/python
mkdir -p "$OUT"
SUM="$OUT/ozet.txt"; : > "$SUM"
fail=0

run() {   # ad, komut...
  local name="$1"; shift
  local t0=$SECONDS
  if "$@" > "$OUT/$name.log" 2>&1; then r="OK"; else r="HATA"; fail=1; fi
  echo "$(date +%H:%M:%S) $r $name ($((SECONDS - t0)) s)" | tee -a "$SUM"
}

run 1-birim "$ROOT/esp_bin/build.sh" test
run 2-derleme-flash "$ROOT/esp_bin/build.sh" -p "$PORT" flash
grep -E "warning:|error:" "$OUT/2-derleme-flash.log" > /dev/null && { echo "   derlemede uyarı/hata satırı var" | tee -a "$SUM"; fail=1; }
for i in $(seq 1 30); do curl -s -m 3 "http://$IP/api/auth" > /dev/null && break; sleep 2; done
run 3-api python3 "$ROOT/2611/HW/test/api/test_api.py" "$IP"
run 4-ekran python3 "$ROOT/esp_bin/gui_check.py" "http://$IP/" "$OUT/ekran"
run 5-gidis-donus python3 "$ROOT/esp_bin/gui_roundtrip.py" "http://$IP/" "$OUT/gidis"
run 6-guvenlik python3 "$ROOT/2611/HW/test/api/test_security.py" "$IP"

for f in 3-api 4-ekran 5-gidis-donus 6-guvenlik; do
  grep -E "HATA|hata:|Traceback|Error" "$OUT/$f.log" | grep -v "^== sonuç: 0 hata" | head -40 | sed "s/^/   [$f] /" >> "$SUM"
done
tail -1 "$OUT/3-api.log" | sed 's/^/   /' >> "$SUM"
tail -1 "$OUT/6-guvenlik.log" | sed 's/^/   /' >> "$SUM"
grep -h "gui_check:" "$OUT/4-ekran.log" "$OUT/5-gidis-donus.log" | sed 's/^/   /' >> "$SUM"
echo "SONUC: $([ $fail = 0 ] && echo TEMIZ || echo HATALI)" | tee -a "$SUM"
exit $fail
