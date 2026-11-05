# 06 — Erken uyarı (alarm) tasarımı

Tarih: 2026-11-05. Durum: kullanıcı onayladı (model C). `yapilacaklar.md` madde 1'in kararıdır; 2611 adım 2.1'de uygulanır.

## Amaç

Sahip, süre dolmadan önce zamanında ve artan aciliyetle uyarılsın; erken dönemde gereksiz uyarı olmasın; 1 dk – 60 gün aralığının tamamında çalışsın; kural sessizce değiştirilmesin.

## ALTE'deki mantık ve sorunları

`k. uyarı: kalan ≤ toplam·(n−k)/(n+1)` — uyarılar tüm süreye eşit aralıkla yayılır. 7 gün/3 uyarı → 5,25 g · 3,5 g · 1,75 g kala: ilk uyarı çok erken, son uyarı çok erken. Ayrıca `warn ≤ değer/2` (birim adedi) kırpması sessiz ve kalıcı ("1 gün" → 0 uyarı).

## Karşılaştırılan yöntemler

A eşit aralık/tüm süre (ALTE) · B eşit aralık/son pencere · **C sona doğru sıklaşan logaritmik pencere (seçildi)** · D elle tanımlı zamanlar · E hazır profiller (C üzerine arayüz katmanı; şimdilik yok).

## Model C

Girdiler (cfg `timer`):

| Alan | Tip | Aralık | Varsayılan | Anlamı |
|---|---|---|---|---|
| `warn_count` | u8 | 1…10 | 3 | uyarı sayısı N |
| `warn_first_s` | u32 | 0 ya da 60…toplam−60 | 0 | ilk uyarı süre dolmadan kaç sn önce; 0 = otomatik |

Hesap (`warn_sched.c`, saf C, masaüstü testli; tüm değerler saniye):

```
T  = toplam süre
W  = warn_first_s ? warn_first_s : min(T/2, 3 gün)            ilk uyarı (kala)
R  = clamp(T/100, 60, 3600)                                    tepki payı
L  = N == 1 ? W : max(W / 2^(N−1), R)                          son uyarı (kala)
k. uyarı (k = 0…N−1):  off_k = W × (L/W)^(k/(N−1))             (N = 1 → off_0 = W)
```

Doğrulama (kayıtta; sessiz kırpma yok):
- `W < T` ve `L ≥ 60` olmalı.
- Ardışık uyarılar arası `off_k − off_{k+1} ≥ 60 sn` olmalı.
- Geçmezse kayıt reddedilir: `validation`, alan `warn_count`, mesajda bu süreye sığan **en çok N** (`warn_max_count(T, W)`).

Örnekler:

| T | N | W | Uyarılar (kala) |
|---|---|---|---|
| 7 gün | 4 | 72 sa (oto) | 72 · 36 · 18 · 9 sa |
| 24 sa | 3 | 12 sa (oto) | 12 · 6 · 3 sa |
| 10 dk | 3 | 5 dk (oto) | 300 · 150 · 75 sn |
| 60 gün | 10 | 72 sa (oto) | 72 · 45 · 28 · 17 · 11 · 6,7 · 4,2 · 2,6 · 1,6 · 1 sa |
| 2 dk | 1 | 60 sn (oto) | 60 sn |

## Çalışma zamanı davranışı

- Eşikler sayaç kurulurken bir kez hesaplanır (`warn_plan_t`: N adet `off_k`); `timer_sm_eval` son tarih aritmetiğiyle `kalan ≤ off_k` olan ilk ateşlenmemiş uyarıyı verir (tek değerlendirmede en fazla bir).
- Ayar sayaç çalışırken değişirse plan yeniden hesaplanır; zaten geçilmiş eşikler ateşlenmeden sayılır.
- Açılışta (kapalı süre sayılmaz): geçilmiş eşikler ateşlenmeden sayılır; ancak `rt.early_pending` (geçilmiş ama teslim edilmemiş uyarı) varsa **yalnız en son** uyarı gönderilir.
- Yeni uyarı, kuyrukta bekleyen/yeniden denenen eski uyarı işlerini iptal eder (kuyruk şişmez).
- Tatil: uyarılar duraklar; tatil bitince tam süreden yeni tur ve yeni plan.
- Erteleme: plan sıfırlanır.

## Mesaj

Yer tutucular (erken uyarı maili, Telegram, webhook/MQTT): `{remaining}` (insan okunur kalan süre, kullanıcı dilinde), `{k}` (1 tabanlı), `{n}`, `{last}` (son uyarıda "1"). Son uyarıda konu başına dil dosyasından "SON UYARI" öneki eklenir. Varsayılan metinler dil dosyasında.

## API ve arayüz

- `PUT /api/timer` yanıtı ve `GET /api/status`: `warn_plan: [off_0, …]` (sn) ve `warn_fired`.
- Doğrulama hatasında `{"code":"validation","field":"warn_count","max":X}`.
- Arayüz: N kaydırıcısı (1–10), "ilk uyarı" seçimi (Otomatik / değer + birim), canlı önizleme ("Uyarılar: 72 sa · 36 sa · 18 sa · 9 sa kala"), sınır aşımında satır içi uyarı.

## Testler (masaüstü)

Örnek tablosunun tamamı; N=1; W elle verildiğinde; R alt/üst sınırları; 60 sn aralık ihlali → `warn_max_count`; yeniden plan + geçilmiş eşik sayımı; açılışta `early_pending`.

## Sonraya (yeni özellik, `yapilacaklar.md`)

Hazır profiller (E); son uyarıda ikinci kişiye haber; mailde tek tıkla erteleme bağlantısı.
