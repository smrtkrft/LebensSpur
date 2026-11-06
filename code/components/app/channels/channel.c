#include <stdio.h>
#include <time.h>
#include "channel.h"
#include "evlog.h"
#include "net_wifi.h"

channel_result_t channel_send(const channel_ctx_t *c, char *err, size_t errlen)
{
    switch (c->job->ch) {
    case CH_SMTP: evlog_mark(EVM_MAIL_SEND, c->job->group); return ch_smtp_send(c, err, errlen);
    case CH_TELEGRAM: evlog_mark(EVM_TG_SEND, 0); return ch_telegram_send(c, err, errlen);
    case CH_WEBHOOK: evlog_mark(EVM_WEBHOOK, 0); return ch_webhook_send(c, err, errlen);
    case CH_MQTT: evlog_mark(EVM_MQTT, 0); return ch_mqtt_send(c, err, errlen);
    default: snprintf(err, errlen, "bilinmeyen kanal"); return CR_PERM;
    }
}

void channel_vars(const channel_ctx_t *c, tmpl_vars_t *v, char *tb, size_t tl, char *rb, size_t rl)
{
    static const char *const st[] = { "triggered", "early_warning", "test" };
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm g;
        gmtime_r(&now, &g);
        strftime(tb, tl, "%Y-%m-%dT%H:%M:%SZ", &g);
    } else {
        snprintf(tb, tl, "+%lus", (unsigned long)evlog_uptime_s());
    }
    uint32_t r = c->remaining_s;
    if (r >= 86400) snprintf(rb, rl, "%lu d %lu h", (unsigned long)(r / 86400), (unsigned long)(r % 86400 / 3600));
    else if (r >= 3600) snprintf(rb, rl, "%lu h %lu min", (unsigned long)(r / 3600), (unsigned long)(r % 3600 / 60));
    else snprintf(rb, rl, "%lu min", (unsigned long)((r + 59) / 60));
    *v = (tmpl_vars_t){ .status = st[c->job->kind < 3 ? c->job->kind : 2], .time = tb,
                        .device = net_wifi_device_name(), .remaining = rb,
                        .k = c->job->kind == JK_EARLY_WARN ? c->job->warn_idx + 1 : 0, .n = c->warn_n };
}
