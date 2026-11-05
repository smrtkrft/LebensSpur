# 2611 — Yeniden yapılandırma süreci

Başlangıç: 2026-11-05. Referans: `ALTE/HW` + `ALTE/GUI` (aktif hat, v0.0.70).

## Hedef

ALTE'nin yaptığı her işi yapan, ama çok daha iyi toparlanmış yeni bir yapı. Yeni özellik yok.

Temel kararlar (kullanıcı, 2026-11-05):

- Program ve altyapının tamamı (firmware + web arayüzü) **yalnızca dahili 4 MB flash'ta**.
- Harici 16 MB flash **yalnızca** kullanıcı verisi, sonradan yapılan yapılandırmalar ve özel dosyalar için.
- GUI'nin harici flash slotlarına kopyalanması ve ayrı GUI OTA'sı kalkar; tek güncelleme kanalı firmware OTA.
- Kod ALTE'den kopyalanabilir ama yapı sıfırdan kurulur.

## Adımlar

| # | Adım | Durum | Çıktı |
|---|------|-------|-------|
| 1 | Kod ve cihaz işlevini anla, haritala, not çıkar | tamam (2026-11-05) | `docs/01-harita/01…07.md` — 6 harita + bulgu kaydı (46 bulgu, 11 ilke, 10 ürün sorusu) |
| 2 | İlk plan | sürüyor | `docs/02-plan-ilk.md` |
| 3 | Plandaki özelliklerin daha iyisi var mı araştır | bekliyor | `docs/03-arastirma.md` |
| 4 | Son plan | bekliyor | `docs/04-plan-son.md` |
| 5 | Uygulama (adım adım, her adım derleme + flash + commit) | bekliyor | `2611/HW`, `2611/GUI` |
| 6 | Denetim, en ince ayrıntıya kadar | bekliyor | `docs/05-denetim.md` |

## Karar günlüğü

| Tarih | Karar | Gerekçe |
|-------|-------|---------|
| 2026-11-05 | Yalnızca dahili flash; harici flash sadece veri | GUI dört kopya tutuluyordu (ota_0, ota_1, Slot A, Slot B); tek sürüm, tek OTA kanalı; harici flash'ın tamamı depolamaya kalır |

## Çalışma kuralları

- Her adım: yap → denetle → düzelt → yeniden denetle → commit. Ara commit atlanmaz.
- Commit mesajları Türkçe. Yazar yalnızca kullanıcının git kimliği.
- Derleme çıktıları `esp_bin/` altında; `2611/` altına üretilen dosya yazılmaz.
- Her adımdan sonra USB'deki cihaz flash'lanır ve açılış logu alınır.
