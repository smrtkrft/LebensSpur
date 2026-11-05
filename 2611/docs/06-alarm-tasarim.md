# 06 — Erken uyarı (alarm) tasarımı

Tarih: 2026-11-05. Durum: kullanıcı onayladı (model C). `yapilacaklar.md` madde 1'in kararıdır; 2611 adım 2.1'de uygulanır.

## Amaç

Sahip, süre dolmadan önce zamanında ve artan aciliyetle uyarılsın; erken dönemde gereksiz uyarı olmasın; 1 dk – 60 gün aralığının tamamında çalışsın; kural sessizce değiştirilmesin.

## ALTE'deki mantık ve sorunları

`k. uyarı: kalan ≤ toplam·(n−k)/(n+1)` — uyarılar tüm süreye eşit aralıkla yayılır. 7 gün/3 uyarı → 5,25 g · 3,5 g · 1,75 g kala: ilk uyarı çok erken, son uyarı çok erken. Ayrıca `warn ≤ değer/2` (birim adedi) kırpması sessiz ve kalıcı ("1 gün" → 0 uyarı).

## Karşılaştırılan yöntemler

A eşit aralık/tüm süre (ALTE) · B eşit aralık/son pencere · **C sona doğru sıklaşan logaritmik pencere (seçildi)** · D elle tanımlı zamanlar · E hazır profiller (C üzerine arayüz katmanı; şimdilik yok).

## Model C (son hali: F1 + L1 + geometrik, 2026-11-05)

İlk ve son uyarı kurala bağlı, aradakiler geometrik dağıtılır. Seçim süreci: önce yarılanan pencere (ilk uyarı T/2) denendi; kullanıcı ilk uyarının çok erken olduğunu ve son uyarının sabit bir "son şans" olması gerektiğini belirtti (24 sa → 23. saat; 60 gün → son gün). Karşılaştırılan son uyarı kuralları: L1 oransal (seçildi), L2 kademe tablosu (sınırda sıçrar), L3 T/100 (60 günde 1 sa: geç), L4 sabit 1 sa. Dağılım: geometrik (seçildi), karesel (son iki uyarı üst üste), eşit (sıklaşma yok). Tamamen elle ayar süre değişince kırıldığı için seçilmedi.

Girdiler (cfg `timer`):

| Alan | Tip | Aralık | Varsayılan | Anlamı |
|---|---|---|---|---|
| `warn_count` | u8 | 1…10 | 3 | uyarı sayısı N |
| `warn_first_s` | u32 | 0 ya da > son uyarı | 0 | ilk uyarı kaç sn kala; 0 = otomatik (N=1'de kullanılmaz) |

Hesap (`components/app/core/warn_sched.c`, saf C; saniye):

```
T  = toplam süre (birimden bağımsız)
F  = clamp(T/10, 15, 60)               en kısa uyarı ve iki uyarı arası en kısa süre
W  = warn_first_s ? warn_first_s : T/4  ilk uyarı (F1: son çeyrek)
L  = max(min(T/24, 1 gün), F)           son uyarı (L1: son şans)
N = 1  → tek uyarı L
N ≥ 2  → off_0 = W, off_{N-1} = L,
         off_k = W·(L/W)^(k/(N−1))       (geometrik: her uyarıda kalan süre aynı oranda azalır)
         sondan başa her aralık ≥ F'ye yaslanır; ilk uyarıya taşarsa aradakiler eşit aralığa düşer
```

Doğrulama (sessiz kırpma yok): `L < T`; N ≥ 2 için `L < W < T` ve `W ≥ L + (N−1)·F`. Geçmezse `validation`, alan `warn_count` (ya da `warn_first_s`), yanıtta `max = warn_max_count(T, W)`.

Örnekler (otomatik):

| T | N | Uyarılar (kala) |
|---|---|---|
| 24 sa | 4 | 6 · 3,3 · 1,8 · 1 sa (18. ve 23. saat) |
| 2 gün | 3 | 12 sa · 4,9 sa · 2 sa |
| 7 gün | 4 | 42 · 23 · 12,7 · 7 sa |
| 60 gün | 4 | 15 · 6,1 · 2,5 · 1 gün |
| 60 gün | 10 | 15 · 11,1 · 8,2 · 6,1 · 4,5 · 3,3 · 2,5 · 1,8 · 1,4 · 1 gün |
| 60 dk | 10 | 15 dk … 2,5 dk |
| 1 dk | 1 | 15 sn |

Kapasite: saat ve gün birimlerinde her değere 10 uyarı sığar; dakikada 1 dk → 1, 2–11 dk → 2, 12–43 dk → 3…9, 44 dk+ → 10.

Doğrulama kanıtı: `test/host/explore_warn.c` 25.190 kombinasyon (3 birim × 1–60 × N 1–10 × otomatik + 13 elle W), kural ihlali 0; `test/host/test_warn_sched.c` kullanıcı örnekleri.

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
