/*
 * core — app_core görevi. Durum makineleri (timer_sm, relay_sm, button_sm, outbox) burada birleşir.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"
#include "button_sm.h"
#include "cfg.h"
#include "ota.h"
#include "core.h"
#include "evlog.h"
#include "io.h"


#define Q_LEN        32
#define CKPT_US      (60LL * 1000000LL)
#define RT_NS        "rt"
#define RT_KEY       "rt"
#define RT_SCHEMA    1
#define SLOW_RT_MS   200

typedef struct {
    uint8_t schema;
    timer_rt_t timer;
    outbox_rt_t outbox;
} rt_blob_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_status_lock;
static app_status_t s_status;
static core_dispatch_t s_dispatch;
static core_status_cb_t s_status_cb;
static esp_timer_handle_t s_tick, s_btn_window;
static uint32_t s_q_drop;

/* yalnız app_core görevinde erişilir */
static timer_sm_t s_timer;
static relay_sm_t s_relay;
static button_sm_t s_btn;
static outbox_t s_ob;
static bool s_link_up, s_delivery_busy, s_relay_out, s_inverted;
static rt_blob_t s_rt_last;
static int64_t s_last_ckpt_us;
static int64_t s_quiet_until_us;               /* bu ana kadar yeni teslimat başlamaz */
static int64_t s_dispatch_at_us;               /* süren teslimatın başlangıcı (bekçi) */

#define NET_SETTLE_US   (10LL * 1000000LL)      /* ağ bağlanınca yerleşme */
#define RELAY_QUIET_US  (2LL * 1000000LL)       /* röle enerjilenirken TLS başlamasın (güç piki) */
#define DELIVERY_HANG_US (8LL * 60 * 1000000LL)  /* 120 sn + 5 ek × 60 sn + pay */

static int64_t now_us(void) { return esp_timer_get_time(); }

/* ---------- kalıcılık ---------- */

static void rt_load(rt_blob_t *rt, bool *have)
{
    *have = false;
    nvs_handle_t h;
    if (nvs_open(RT_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof *rt;
    if (nvs_get_blob(h, RT_KEY, rt, &len) == ESP_OK && len == sizeof *rt && rt->schema == RT_SCHEMA) *have = true;
    nvs_close(h);
}

static void rt_save(bool force)
{
    rt_blob_t rt = { .schema = RT_SCHEMA };
    int64_t now = now_us();
    timer_sm_to_rt(&s_timer, now, &rt.timer);
    outbox_to_rt(&s_ob, now, &rt.outbox);
    if (!force && memcmp(&rt, &s_rt_last, sizeof rt) == 0) return;
    nvs_handle_t h;
    if (nvs_open(RT_NS, NVS_READWRITE, &h) != ESP_OK) return;
    int64_t t0 = now_us();
    esp_err_t r = nvs_set_blob(h, RT_KEY, &rt, sizeof rt);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    int64_t ms = (now_us() - t0) / 1000;
    if (r != ESP_OK) evlog_write(EVL_ERR, "sys", "rt kaydi: %s", esp_err_to_name(r));
    else s_rt_last = rt;
    if (ms > SLOW_RT_MS) evlog_write(EVL_WARN, "sys", "rt kaydi yavas: %lld ms", ms);
    s_last_ckpt_us = now;
}

/* ---------- yardımcılar ---------- */

static uint32_t total_from_cfg(const cfg_timer_t *t)
{
    static const uint32_t unit_s[] = { 60u, 3600u, 86400u };
    return (uint32_t)t->value * unit_s[t->unit < 3 ? t->unit : 2];
}

static ob_channels_t channels(bool trigger)
{
    cfg_actions_t a;
    cfg_copy_section(CFG_SEC_ACTIONS, &a);
    if (trigger) return (ob_channels_t){ a.trig_mail, false, a.trig_webhook, a.trig_mqtt };
    return (ob_channels_t){ a.early_mail, a.early_telegram, a.early_webhook, a.early_mqtt };
}

static void relay_cfg_load(void)
{
    cfg_relay_t c;
    cfg_copy_section(CFG_SEC_RELAY, &c);
    relay_cfg_t rc = { c.delay_s, c.pulse_s, c.duration_s };
    relay_sm_set_cfg(&s_relay, &rc);
    s_inverted = c.inverted;
}

static void relay_apply(int64_t now)
{
    bool on = relay_sm_eval(&s_relay, now);
    io_relay_set(on, s_inverted);
    if (on != s_relay_out) {
        s_relay_out = on;
        evlog_mark(EVM_RELAY, on);
        if (on && s_quiet_until_us < now + RELAY_QUIET_US) s_quiet_until_us = now + RELAY_QUIET_US;
    }
}

static void publish(int64_t now)
{
    app_status_t st = { 0 };
    st.state = s_timer.state;
    st.total_s = s_timer.total_s;
    st.remaining_s = timer_sm_remaining_s(&s_timer, now);
    st.vac_total_s = s_timer.vac_total_s;
    st.vac_remaining_s = timer_sm_vac_remaining_s(&s_timer, now);
    st.warn_n = s_timer.plan.n;
    st.warn_fired = s_timer.warn_fired;
    memcpy(st.warn_off_s, s_timer.plan.off_s, sizeof st.warn_off_s);
    st.relay_on = s_relay_out;
    st.relay_remaining_s = relay_sm_remaining_s(&s_relay, now);
    st.trigger = outbox_trigger_summary(&s_ob);
    st.link_up = s_link_up;
    st.q_drop = s_q_drop;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    st.last_checkin_src = s_status.last_checkin_src;
    st.last_checkin_up_s = s_status.last_checkin_up_s;
    st.version = (uint16_t)(s_status.version + 1);
    /* zamandan türeyen alanlar (kalan süreler) her saniye değişir: SSE yalnız olayda gitsin,
     * istemci kalan süreyi kendi saatiyle ilerletir (güç kısıtı) */
    app_status_t a = st, b = s_status;
    a.remaining_s = b.remaining_s = 0;
    a.vac_remaining_s = b.vac_remaining_s = 0;
    a.relay_remaining_s = b.relay_remaining_s = 0;
    bool changed = memcmp(&a, &b, offsetof(app_status_t, version)) != 0;
    s_status.remaining_s = st.remaining_s;          /* okuyanlar güncel kalanı görsün */
    s_status.vac_remaining_s = st.vac_remaining_s;
    s_status.relay_remaining_s = st.relay_remaining_s;
    if (changed) s_status = st;
    xSemaphoreGive(s_status_lock);
    if (changed && s_status_cb) s_status_cb();
}

static void dispatch_next(int64_t now)
{
    if (s_delivery_busy || !s_dispatch || now < s_quiet_until_us) return;
    ob_job_t *j = outbox_take_next(&s_ob, now, s_link_up);
    if (!j) return;
    if (s_dispatch(j, s_ob.trigger_seq, timer_sm_remaining_s(&s_timer, now), s_timer.plan.n)) {
        s_delivery_busy = true;
        s_dispatch_at_us = now;
    } else {
        j->state = JS_PENDING;                       /* delivery hazır değil: sonra */
    }
}

/* ---------- olaylar ---------- */

static void do_checkin(checkin_source_t src, int64_t now)
{
    static const char *const names[] = { "dugme", "web", "api_anahtari" };
    int r = timer_sm_checkin(&s_timer, now);
    relay_sm_off(&s_relay);                           /* her erteleme röleyi söndürür (karar 3) */
    relay_apply(now);
    int cancelled = outbox_cancel_pending_trigger(&s_ob);   /* karar 4 */
    evlog_mark(EVM_CHECKIN, src);
    if (r < 0) {
        evlog_write(EVL_WARN, "timer", "erteleme (%s): ayarlı sure yok", names[src]);
    } else {
        evlog_write(EVL_INFO, "timer", "erteleme (%s)%s%s", names[src], r == 1 ? ": tatilde, sayac etkilenmedi" : "",
                    cancelled ? ", bekleyen tetik mesajlari iptal" : "");
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        s_status.last_checkin_src = src;
        s_status.last_checkin_up_s = evlog_uptime_s();
        xSemaphoreGive(s_status_lock);
    }
    rt_save(false);
}

static void on_timer_actions(unsigned a, int64_t now)
{
    if (a & TA_TRIGGER) {
        cfg_actions_t act;
        cfg_copy_section(CFG_SEC_ACTIONS, &act);
        evlog_mark(EVM_TIMER_EXPIRED, 0);
        ota_abort();                                    /* tetikleme önceliklidir: süren indirme durur */
        ob_channels_t c = channels(true);
        int n = outbox_plan_trigger(&s_ob, &c, cfg_group_mask(), now);
        if (act.trig_relay) relay_sm_trigger(&s_relay, now, 0);
        evlog_write(EVL_WARN, "timer", "sure doldu: protokol tetiklendi (%d mesaj, role %s)", n,
                    act.trig_relay ? "acik" : "kapali");
    }
    if (a & TA_VACATION_END) evlog_write(EVL_INFO, "timer", "tatil bitti, sayac tam sureden basladi");
    if (a & TA_PERSIST) rt_save(false);
}

static void handle(const core_event_t *ev)
{
    int64_t now = now_us();
    switch (ev->type) {
    case EV_TICK: {
        uint8_t widx = 0;
        unsigned a = timer_sm_eval(&s_timer, now, &widx);
        if (a & TA_WARN) {
            ob_channels_t c = channels(false);
            int n = outbox_plan_early(&s_ob, &c, widx, now);
            evlog_write(EVL_WARN, "timer", "erken uyari %u/%u (%lu sn kala, %d mesaj)", widx + 1, s_timer.plan.n,
                        (unsigned long)timer_sm_remaining_s(&s_timer, now), n);
            a &= ~(unsigned)TA_WARN;
        }
        on_timer_actions(a, now);
        if (s_timer.state == TS_RUNNING && now - s_last_ckpt_us >= CKPT_US) rt_save(false);
        if (s_delivery_busy && now - s_dispatch_at_us > DELIVERY_HANG_US) {
            evlog_write(EVL_ERR, "sys", "teslimat 8 dk'dir yanit vermiyor: yeniden baslatma");
            rt_save(true);
            sys_restart(RST_DELIVERY_HANG);
        }
        break;
    }
    case EV_BUTTON_PRESS:
        evlog_mark(EVM_BUTTON, 0);
        if (button_sm_press(&s_btn, now) == BA_FACTORY1) {
            esp_timer_stop(s_btn_window);
            core_post(&(core_event_t){ .type = EV_FACTORY_RESET, .u.factory.level = 1 });
        } else {
            esp_timer_stop(s_btn_window);
            esp_timer_start_once(s_btn_window, BUTTON_WINDOW_MS * 1000);
        }
        break;
    case EV_BUTTON_WINDOW:
        switch (button_sm_window_end(&s_btn)) {
        case BA_CHECKIN: do_checkin(CHK_BUTTON, now); break;
        case BA_RESTART:
            evlog_write(EVL_INFO, "user", "dugme 3x: yeniden baslatma");
            rt_save(true);
            sys_restart(RST_USER_BUTTON);
            break;
        default: break;
        }
        break;
    case EV_CHECKIN:
        do_checkin(ev->u.checkin.src, now);
        break;
    case EV_TIMER_CANCEL:
        if (timer_sm_cancel(&s_timer, now) == 0) {
            outbox_cancel_pending_trigger(&s_ob);
            evlog_write(EVL_INFO, "timer", "sayac durduruldu");
            rt_save(false);
        }
        break;
    case EV_VACATION_SET:
        if (timer_sm_vacation(&s_timer, ev->u.vacation.days, now) == 0) {
            evlog_write(EVL_INFO, "timer", "tatil %u gun", ev->u.vacation.days);
            rt_save(false);
        }
        break;
    case EV_VACATION_CANCEL:
        if (timer_sm_vacation_cancel(&s_timer, now) == 0) {
            evlog_write(EVL_INFO, "timer", "tatil iptal, sayac tam sureden basladi");
            rt_save(false);
        }
        break;
    case EV_RELAY_TEST:
        relay_sm_trigger(&s_relay, now, ev->u.relay_test.duration_s ? ev->u.relay_test.duration_s : 5);
        evlog_write(EVL_INFO, "relay", "role testi");
        break;
    case EV_TEST_SEND:
        outbox_plan_test(&s_ob, (channel_t)ev->u.test.ch, ev->u.test.job_id, now);
        break;
    case EV_DELIVERY_RESULT:
        s_delivery_busy = false;
        outbox_on_result(&s_ob, ev->u.delivery.seq, (channel_result_t)ev->u.delivery.result, now, esp_random());
        rt_save(false);
        break;
    case EV_NET_LINK:
        s_link_up = ev->u.link.up;
        if (s_link_up && s_quiet_until_us < now + NET_SETTLE_US) s_quiet_until_us = now + NET_SETTLE_US;
        break;
    case EV_CFG_CHANGED:
        if (ev->u.cfg.section == CFG_SEC_TIMER) {
            cfg_timer_t t;
            cfg_copy_section(CFG_SEC_TIMER, &t);
            uint32_t total = total_from_cfg(&t);
            warn_err_t e = total != s_timer.total_s
                               ? timer_sm_set(&s_timer, total, t.warn_count, t.warn_first_s, now)
                               : timer_sm_set_warn(&s_timer, t.warn_count, t.warn_first_s, now);
            if (e != WARN_OK) evlog_write(EVL_ERR, "timer", "uyari plani gecersiz (%d)", e);
            else evlog_write(EVL_INFO, "timer", "sayac ayari: %lu sn, %u uyari", (unsigned long)total, t.warn_count);
            rt_save(false);
        } else if (ev->u.cfg.section == CFG_SEC_RELAY) {
            relay_cfg_load();
            relay_apply(now);
        }
        outbox_cfg_changed(&s_ob, now);
        break;
    case EV_FACTORY_RESET:
        evlog_write(EVL_WARN, "user", "fabrika sifirlama (kademe %u)", ev->u.factory.level);
        cfg_factory_reset();
        sys_restart(RST_FACTORY);
        break;
    }
    relay_apply(now);
    dispatch_next(now);
    publish(now);
}

/* ---------- görev ve kurulum ---------- */

static void task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    core_event_t ev;
    for (;;) {
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) handle(&ev);
        esp_task_wdt_reset();
    }
}

static void tick_cb(void *arg) { (void)arg; core_post(&(core_event_t){ .type = EV_TICK }); }
static void btn_window_cb(void *arg) { (void)arg; core_post(&(core_event_t){ .type = EV_BUTTON_WINDOW }); }
static void IRAM_ATTR btn_isr(void) { core_post_from_isr(&(core_event_t){ .type = EV_BUTTON_PRESS }); }
static void cfg_changed(cfg_section_t s) { core_post(&(core_event_t){ .type = EV_CFG_CHANGED, .u.cfg.section = (uint8_t)s }); }

esp_err_t core_start(void)
{
    s_q = xQueueCreate(Q_LEN, sizeof(core_event_t));
    s_status_lock = xSemaphoreCreateMutex();
    if (!s_q || !s_status_lock) return ESP_ERR_NO_MEM;

    int64_t now = now_us();
    cfg_timer_t t;
    cfg_copy_section(CFG_SEC_TIMER, &t);
    rt_blob_t rt;
    bool have;
    rt_load(&rt, &have);
    timer_sm_init(&s_timer, have ? &rt.timer : NULL, t.warn_count, t.warn_first_s, now);
    if (have) outbox_from_rt(&s_ob, &rt.outbox, now);
    else outbox_init(&s_ob);
    s_rt_last = have ? rt : (rt_blob_t){ 0 };
    relay_sm_init(&s_relay, &(relay_cfg_t){ 0 });
    relay_cfg_load();
    io_relay_set(false, s_inverted);                  /* cfg okundu: doğru boşta seviyesi */
    s_last_ckpt_us = now;

    ob_summary_t sum = outbox_trigger_summary(&s_ob);
    evlog_write(EVL_INFO, "timer", "sayac: durum %d, %lu/%lu sn, %u uyari%s", s_timer.state,
                (unsigned long)timer_sm_remaining_s(&s_timer, now), (unsigned long)s_timer.total_s, s_timer.plan.n,
                sum.pending ? ", bekleyen tetik mesajlari devam edecek" : "");

    publish(now);                                     /* ilk tikten önce okuyan doğru durumu görsün */
    cfg_set_change_cb(cfg_changed);
    const esp_timer_create_args_t ta = { .callback = tick_cb, .name = "core_tick",
                                         .skip_unhandled_events = true };
    const esp_timer_create_args_t tb = { .callback = btn_window_cb, .name = "btn_win" };
    if (esp_timer_create(&ta, &s_tick) != ESP_OK || esp_timer_create(&tb, &s_btn_window) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(task, "app_core", 6 * 1024, NULL, 8, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    esp_timer_start_periodic(s_tick, 1000000);
    esp_err_t r = io_button_init(btn_isr);
    if (r != ESP_OK) evlog_write(EVL_ERR, "sys", "dugme: %s", esp_err_to_name(r));
    return ESP_OK;
}

esp_err_t core_post(const core_event_t *ev)
{
    if (s_q && xQueueSend(s_q, ev, 0) == pdTRUE) return ESP_OK;
    s_q_drop++;
    return ESP_ERR_NO_MEM;
}

void IRAM_ATTR core_post_from_isr(const core_event_t *ev)
{
    BaseType_t hp = pdFALSE;
    if (!s_q || xQueueSendFromISR(s_q, ev, &hp) != pdTRUE) s_q_drop++;
    if (hp) portYIELD_FROM_ISR();
}

void core_get_status(app_status_t *out)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_lock);
}

void core_set_dispatch(core_dispatch_t fn) { s_dispatch = fn; }
void core_set_status_cb(core_status_cb_t fn) { s_status_cb = fn; }
void core_quiet_for(uint32_t seconds)
{
    int64_t t = now_us() + (int64_t)seconds * 1000000LL;
    if (t > s_quiet_until_us) s_quiet_until_us = t;      /* tek yazar sayılır: açılışta, görev başlamadan */
}
