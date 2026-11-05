/*
 * outbox — teslimat kuyruğunun politikası (saf C). Gönderimi delivery görevi yapar;
 * burada yalnız "ne, ne zaman, kaç kez" kararları var (docs/04-plan-son.md §2.8, karar K4).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { JK_TRIGGER, JK_EARLY_WARN, JK_TEST } job_kind_t;          /* öncelik sırası */
typedef enum { CH_SMTP, CH_TELEGRAM, CH_WEBHOOK, CH_MQTT, CH__COUNT } channel_t;
typedef enum { JS_FREE, JS_PENDING, JS_RUNNING, JS_DONE, JS_FAILED, JS_CANCELLED } job_state_t;
typedef enum { CR_OK, CR_RETRY, CR_PERM, CR_CONFIG } channel_result_t;

#define OUTBOX_MAX     24
#define OUTBOX_GROUPS  10
#define OUTBOX_OWNER   0xFF          /* erken uyarı maili: sahibin kendi adresi */
#define OUTBOX_GIVEUP_S (72u * 3600u)

typedef struct {
    uint16_t seq;
    uint8_t  kind, ch, group, warn_idx, attempt, state;
    bool     config_blocked;
    int64_t  not_before_us;
} ob_job_t;

typedef struct {
    ob_job_t jobs[OUTBOX_MAX];
    uint16_t next_seq;
    uint16_t trigger_seq;            /* her tetiklemede artar (Message-ID, Idempotency-Key) */
    bool     trigger_active;
    int64_t  trigger_at_us;
} outbox_t;

typedef struct {                     /* tetikleme/uyarı için açık kanallar (cfg actions) */
    bool mail, telegram, webhook, mqtt;
} ob_channels_t;

typedef struct {                     /* kalıcı kayıt: yalnız tetikleme ve son erken uyarı */
    uint16_t trigger_seq;
    uint8_t  trig_active;
    uint8_t  early_idx;              /* 0xFF = bekleyen uyarı yok */
    uint32_t since_trigger_s;
    uint16_t mail_mask;              /* bekleyen grup mailleri */
    uint8_t  other_mask;             /* bit0 webhook, bit1 mqtt */
    uint8_t  early_mask;             /* bekleyen erken uyarı kanalları (bit = channel_t) */
    uint8_t  attempts[OUTBOX_GROUPS + 2];
} outbox_rt_t;

typedef struct { uint8_t pending, done, failed, cancelled; } ob_summary_t;

void  outbox_init(outbox_t *o);
int   outbox_plan_trigger(outbox_t *o, const ob_channels_t *c, uint16_t group_mask, int64_t now_us);
int   outbox_plan_early(outbox_t *o, const ob_channels_t *c, uint8_t warn_idx, int64_t now_us);
int   outbox_plan_test(outbox_t *o, channel_t ch, int64_t now_us);     /* iş seq'i, yer yoksa -1 */
/* Sıradaki iş (ağ yoksa NULL). Seçilen iş RUNNING olur. */
ob_job_t *outbox_take_next(outbox_t *o, int64_t now_us, bool link_up);
void  outbox_on_result(outbox_t *o, uint16_t seq, channel_result_t r, int64_t now_us, uint32_t rnd);
int   outbox_cancel_pending_trigger(outbox_t *o);                     /* erteleme: başlamamış tetik işleri */
void  outbox_cfg_changed(outbox_t *o, int64_t now_us);                /* yapılandırma hatasıyla bekleyenler hemen */
int64_t outbox_retry_delay_us(job_kind_t k, uint8_t attempt, uint32_t rnd);   /* attempt: başarısız deneme sayısı */
int64_t outbox_next_due_us(const outbox_t *o);                        /* en yakın not_before; yoksa INT64_MAX */
ob_summary_t outbox_trigger_summary(const outbox_t *o);
const ob_job_t *outbox_find(const outbox_t *o, uint16_t seq);
void  outbox_to_rt(const outbox_t *o, int64_t now_us, outbox_rt_t *rt);
void  outbox_from_rt(outbox_t *o, const outbox_rt_t *rt, int64_t now_us);
