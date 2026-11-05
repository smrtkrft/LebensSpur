/*
 * evlog_classify — açılış/kapanış nedeni sınıflandırması (saf; masaüstünde test edilir).
 * Tablo ALTE sys_log.c ile birebir; metinler GUI'de dil anahtarlarıyla çevrilir
 * (cat.<cat>, cause.<cause>, hint.<hint>).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* esp_reset_reason_t'nin IDF'siz karşılığı (evlog.c eşler). */
typedef enum {
    EVR_UNKNOWN, EVR_POWERON, EVR_EXT, EVR_SW, EVR_PANIC, EVR_INT_WDT, EVR_TASK_WDT, EVR_WDT,
    EVR_DEEPSLEEP, EVR_BROWNOUT, EVR_SDIO, EVR_USB, EVR_JTAG, EVR_EFUSE, EVR_PWR_GLITCH, EVR_CPU_LOCKUP,
} evlog_rr_t;

/* sys_restart() nedenleri (RTC'de saklanır). */
typedef enum {
    RST_NONE, RST_GENERIC, RST_USER_WEB, RST_USER_BUTTON, RST_FACTORY, RST_OTA, RST_DELIVERY_HANG,
} restart_reason_t;

/* RTC'deki "son olay" işaretleri. */
typedef enum {
    EVM_NONE, EVM_BOOT, EVM_AP_CLIENT_JOIN, EVM_AP_CLIENT_LEAVE, EVM_STA_CONNECTING, EVM_STA_GOT_IP,
    EVM_STA_LOST, EVM_GUI_SERVE, EVM_MAIL_SEND, EVM_TG_SEND, EVM_WEBHOOK, EVM_MQTT, EVM_OTA, EVM_TX_POWER,
    EVM_RELAY, EVM_TIMER_EXPIRED, EVM_BUTTON, EVM_CHECKIN, EVM__COUNT
} evlog_mark_t;

typedef struct {
    const char *cat;     /* power | software | user | other | unknown */
    const char *cause;
    const char *hint;    /* "" = yok */
} evlog_cause_t;

#define EVLOG_LOAD_PEAK_WINDOW_S 30u   /* brownout'u yük olayına bağlama penceresi */
#define EVLOG_MIN_RUN_S          5u    /* bundan kısa çalışma "kısa açılış" */

evlog_cause_t evlog_classify(evlog_rr_t rr, bool rtc_ok, restart_reason_t rst, evlog_mark_t last_mark,
                             uint32_t mark_age_s);
const char *evlog_mark_name(evlog_mark_t m);
bool evlog_mark_is_load_peak(evlog_mark_t m);
/* Kısa açılış: güç/yazılım nedenlerinde 1., 10., 20. ... kayıt yazılır (boot döngüsü görünür kalsın). */
bool evlog_short_boot_should_log(const char *cat, uint32_t short_count);
