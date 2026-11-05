/* evlog_classify — ALTE sys_log.c classify() tablosunun saf karşılığı. */
#include <string.h>
#include "evlog_classify.h"

static const char *const s_mark_names[EVM__COUNT] = {
    "NONE", "BOOT", "AP_CLIENT_JOIN", "AP_CLIENT_LEAVE", "STA_CONNECTING", "STA_GOT_IP",
    "STA_LOST", "GUI_SERVE", "MAIL_SEND", "TG_SEND", "WEBHOOK", "MQTT", "OTA", "TX_POWER",
    "RELAY", "TIMER_EXPIRED", "BUTTON", "CHECKIN",
};

const char *evlog_mark_name(evlog_mark_t m)
{
    return (unsigned)m < EVM__COUNT ? s_mark_names[m] : "NONE";
}

bool evlog_mark_is_load_peak(evlog_mark_t m)
{
    switch (m) {
    case EVM_BOOT: case EVM_AP_CLIENT_JOIN: case EVM_GUI_SERVE: case EVM_STA_CONNECTING:
    case EVM_STA_GOT_IP: case EVM_MAIL_SEND: case EVM_TG_SEND: case EVM_WEBHOOK: case EVM_MQTT:
    case EVM_OTA: case EVM_RELAY:
        return true;
    default:
        return false;
    }
}

#define R(c, k, h) (evlog_cause_t){ c, k, h }

evlog_cause_t evlog_classify(evlog_rr_t rr, bool rtc_ok, restart_reason_t rst, evlog_mark_t last_mark,
                             uint32_t mark_age_s)
{
    switch (rr) {
    case EVR_BROWNOUT:
        return R("power", "brownout",
                 (rtc_ok && evlog_mark_is_load_peak(last_mark) && mark_age_s <= EVLOG_LOAD_PEAK_WINDOW_S)
                     ? "brownout_load" : "brownout_other");
    case EVR_PWR_GLITCH: return R("power", "pwr_glitch", "pwr_glitch");
    case EVR_POWERON:
        return rtc_ok ? R("power", "poweron_rst", "poweron_rst") : R("power", "power_loss", "power_loss");
    case EVR_PANIC:      return R("software", "panic", "panic");
    case EVR_INT_WDT:    return R("software", "int_wdt", "int_wdt");
    case EVR_TASK_WDT:   return R("software", "task_wdt", "task_wdt");
    case EVR_WDT:        return R("software", "wdt", "");
    case EVR_CPU_LOCKUP: return R("software", "cpu_lockup", "");
    case EVR_SW:
        switch (rtc_ok ? rst : RST_NONE) {
        case RST_USER_WEB:       return R("user", "user_restart", "");
        case RST_USER_BUTTON:    return R("user", "button_restart", "");
        case RST_FACTORY:        return R("user", "factory_reset", "");
        case RST_OTA:            return R("user", "ota_fw", "");
        case RST_DELIVERY_HANG:  return R("software", "delivery_hang", "");
        default:                 return R("software", "sw_restart", "");
        }
    case EVR_DEEPSLEEP: return R("other", "deepsleep", "");
    case EVR_EXT:       return R("other", "ext_pin", "");
    case EVR_SDIO:      return R("other", "sdio", "");
    case EVR_USB:       return R("other", "usb", "");
    case EVR_JTAG:      return R("other", "jtag", "");
    case EVR_EFUSE:     return R("other", "efuse", "");
    default:            return R("unknown", "unknown", "");
    }
}

bool evlog_short_boot_should_log(const char *cat, uint32_t n)
{
    bool serious = strcmp(cat, "power") == 0 || strcmp(cat, "software") == 0;
    return serious && (n == 1 || n % 10 == 0);
}
