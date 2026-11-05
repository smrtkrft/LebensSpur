# Adım 6 — arayüzün beklediği sunucu uçları

Adım 5'te arayüz (ALTE tasarımı) bu uçlara göre yazıldı; adım 6'da sunucu tarafı bu biçimle uygulanır.
Zarf her yerde `{"ok":true,"data":…}` / `{"ok":false,"error":{code,message,field}}`. Yazma uçları `X-LS-Request: 1` ister.

| Uç | Gövde | Yanıt `data` | Arayüzde |
|---|---|---|---|
| `GET /api/device` | — | `chip, rev, cores, cpu_mhz, flash_kb, slot_kb, app_kb, heap_total, heap_free, heap_min, ext_mb, log{state,total,used}, data{state,total,used}, tx_q, tx_max_q, boot_count, last_cause, last_cat, prev_uptime` | Bilgi › Cihaz |
| `GET /api/logs` | — | `boot_count, last_cause, last_cat, prev_uptime, short_boots, logs[]` (en yeni önce; `/log/events.jsonl` + `.1` satırları, boot/event kayıtları aynen) | Bilgi › Günlükler |
| `DELETE /api/logs` | — | `null` | Günlükleri temizle |
| `POST /api/device/restart` | — | `null` (yanıttan sonra `sys_restart(USER_WEB)`) | Sistem › Yeniden başlat |
| `POST /api/device/factory-reset` | `{level:1\|2}` | `null`; 2'de `/data` tam silme sys_worker işinde, sonra yeniden başlatma | Sistem › Fabrika ayarı |
| `GET /api/remote/key` | — | `{key}` düz metin (yalnız oturumlu) | Uzaktan sıfırlama |
| `POST /api/remote/key` | — | `{key}` yeni 26 kr base32 | Yeni anahtar |
| `GET /api/reset?key=` | — | erteleme (`CHK_API_KEY`); hız sınırı 5 hatalı/dk → 1 dk kilit | dış otomasyon |
| `GET /api/ota` | — | `version, partition, prev_version?, state(idle\|running\|ready\|failed)` | Güncelleme |
| `POST /api/ota/check` | `{source:"official"}` ya da `{source:"custom",url}` (yalnız https) | `{available, version}` | Denetle |
| `POST /api/ota/start` | aynı | `{job_id}`; ilerleme `/api/jobs/{id}` (`pct, message`) | İndir ve kur |
| `POST /api/ota/rollback` | — | `null` (sonraki açılış önceki bölüm) | Geri al |

Sunucuya taşınacak doğrulamalar (arayüz şimdilik kendi denetliyor):
- `device.ap_pass` boş değilse ≥ 8 karakter (WPA2 kuralı); sunucu da reddetmeli.
- Gizli alan boş gönderilince korunuyor → AP parolasını **kaldırmanın** yolu yok. Adım 6: `DELETE /api/config/device/ap_pass` ya da `{"ap_pass":null}` ile açık temizleme.
- Webhook başlıkları JSON nesnesi olmalı (`ch_http.c` cJSON ile ayrıştırıyor); sunucu kayıtta da denetlemeli.
