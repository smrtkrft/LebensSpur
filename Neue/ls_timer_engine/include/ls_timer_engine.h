#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// ls_timer_engine — LebensSpur countdown state machine
//
// Cihazın core iş mantığı. Kullanıcı belirlediği süre boyunca herhangi
// bir reset gelmezse (fiziksel buton, API reset, SKAPP komutu) cihaz
// "tetiklendi" durumuna geçer ve alt sistemleri (mail groups, relay,
// outbound API) event bus üzerinden uyandırır.
//
// State machine (basit, 4 durum):
//
//   ┌───────────┐  timer.set/start          ┌────────────┐
//   │ INACTIVE  │ ────────────────────────► │ COUNTDOWN  │
//   │           │ ◄──────────────────────── │            │
//   └───────────┘        timer.stop         └─────┬──────┘
//          ▲                                      │
//          │                                      │
//          │ deadline'a ulaşıldı                  │ vacation.set N
//          │                                      ▼
//   ┌──────┴──────┐  manual restart      ┌────────────┐
//   │ TRIGGERED   │ ◄─── (start/reset)   │  VACATION  │
//   │             │ ─────────────────►   │            │
//   └─────────────┘                      └─────┬──────┘
//                                              │
//                          vacation_end veya cancel
//                          (kalan süre korunur)
//                                              ▼
//                                        ┌────────────┐
//                                        │ COUNTDOWN  │
//                                        └────────────┘
//
// Olay yayını (event bus):
//   timer.state      {"state":"...","remaining_sec":N}
//                    her state geçişinde
//   timer.tick       {"remaining_sec":N}
//                    countdown sırasında her saniye
//   timer.alarm      {"index":i,"of":N,"remaining_sec":K}
//                    her alarm eşiği geçildiğinde (sondan geriye 1 birim arayla)
//   timer.triggered  {"duration_sec":N}
//                    deadline'a ulaşıldığında, bir kere
//   timer.reset      {"by":"manual"|"api"}
//                    countdown sıfırlandığında (kullanıcı / reset API)
//   timer.vacation   {"active":true,"end_epoch":N} / {"active":false}
//                    vacation moduna giriş/çıkış
//
// CLI komutları (kayıt component'in init'inde):
//   timer.set, timer.start, timer.stop, timer.reset, timer.status,
//   timer.get, vacation.set, vacation.cancel
//
// NVS persistence: config + state + deadline + vacation NVS'e yazılır,
// reboot sonrası süre geçişi wall-clock üzerinden yeniden hesaplanır.
// Wall time'ın SKAPP tarafından `time.set` ile push'lanması gerekir;
// epoch yoksa cihaz COUNTDOWN'a girse de tick durur, time.set sonrası
// devam eder. Bu pattern BF webhook root-cause fix'inden mirastır.
// =====================================================================

typedef enum {
    LS_TIMER_INACTIVE  = 0,
    LS_TIMER_COUNTDOWN = 1,
    LS_TIMER_VACATION  = 2,
    LS_TIMER_TRIGGERED = 3,
} ls_timer_state_t;

typedef enum {
    LS_TIMER_UNIT_MINUTE = 0,
    LS_TIMER_UNIT_HOUR   = 1,
    LS_TIMER_UNIT_DAY    = 2,
} ls_timer_unit_t;

typedef struct {
    ls_timer_unit_t unit;          // day / hour / minute
    uint16_t        value;         // 1..60 (her unit için aynı sınır)
    uint8_t         alarm_count;   // 0..10 alarm sayısı (sondan geriye 1 birim arayla)
} ls_timer_config_t;

typedef struct {
    ls_timer_state_t state;
    uint32_t         remaining_sec;     // COUNTDOWN'da kalan saniye, diğer durumlarda 0
    int64_t          deadline_epoch;    // COUNTDOWN'da deadline (epoch sec), diğer durumlarda 0
    int64_t          vacation_end_epoch;// VACATION'da bitiş (epoch sec), diğer durumlarda 0
    uint32_t         vacation_remaining_sec;  // VACATION'a girilirken kalan countdown'u korur
    uint16_t         alarms_fired_mask; // bit i set ise i. alarm tetiklenmiş
} ls_timer_status_t;

// Init component: NVS'ten config + state yükle, CLI komutlarını kaydet,
// 1 Hz tick task'ını başlat. main.c'de sk_wifi_init / sk_api_init'ten sonra
// çağrılır (event bus, NVS, time(NULL) hazır olmalı).
esp_err_t ls_timer_engine_init(void);

// Snapshot anlık durum — okuma için lock'suz, opportunistic.
void ls_timer_engine_status(ls_timer_status_t *out);

// Config snapshot. NVS'ten boot'ta yüklenen değer; CLI ile değiştirilince
// hem RAM hem NVS güncellenir.
void ls_timer_engine_config(ls_timer_config_t *out);

// State string (CLI / status provider için). "inactive" / "countdown" /
// "vacation" / "triggered" / "?".
const char *ls_timer_engine_state_str(void);

// Unit string. "minute" / "hour" / "day" / "?".
const char *ls_timer_engine_unit_str(ls_timer_unit_t unit);

#ifdef __cplusplus
}
#endif
