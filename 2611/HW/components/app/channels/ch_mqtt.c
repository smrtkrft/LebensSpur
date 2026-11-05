/*
 * ch_mqtt — tek yayın MQTT 3.1.1 istemcisi (net_tls üzerinde; esp-mqtt görev yarattığı için kullanılmaz, K3).
 * mqtt://host[:1883] ya da mqtts://host[:8883]; QoS1, PUBACK beklenir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "cfg.h"
#include "channel.h"
#include "mqtt_pkt.h"
#include "net_tls.h"
#include "net_wifi.h"

static bool parse_url(const char *url, bool *tls, char *host, size_t hcap, uint16_t *port)
{
    const char *p;
    if (strncmp(url, "mqtts://", 8) == 0) { *tls = true; p = url + 8; *port = 8883; }
    else if (strncmp(url, "mqtt://", 7) == 0) { *tls = false; p = url + 7; *port = 1883; }
    else return false;
    size_t n = strcspn(p, ":/");
    if (!n || n >= hcap) return false;
    memcpy(host, p, n);
    host[n] = 0;
    if (p[n] == ':') *port = (uint16_t)atoi(p + n + 1);
    return *port != 0;
}

/* bir MQTT paketi: önce sabit başlık (tür + 1..4 baytlık kalan uzunluk), sonra gövde */
static int read_packet(net_conn_t *c, uint8_t *b, size_t cap, int64_t dl)
{
    size_t got = 0, hdr = 0;
    int rem = -1;
    while (rem < 0) {                                  /* başlık: bayt bayt (en çok 5) */
        if (got >= 5 || net_read(c, b + got, 1, dl) != 1) return -1;
        got++;
        if (got >= 2) rem = mqtt_remaining_len(b, got, &hdr);
    }
    size_t total = hdr + (size_t)rem;
    if (total > cap) return -1;
    while (got < total) {
        int n = net_read(c, b + got, total - got, dl);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return (int)got;
}

channel_result_t ch_mqtt_send(const channel_ctx_t *ctx, char *err, size_t errlen)
{
    static cfg_mqtt_t mq;
    cfg_copy_section(CFG_SEC_MQTT, &mq);
    bool tls;
    char host[96];
    uint16_t port;
    if (!mq.url[0] || !mq.topic[0] || !parse_url(mq.url, &tls, host, sizeof host, &port)) {
        return (snprintf(err, errlen, "mqtt ayari yok/gecersiz"), CR_CONFIG);
    }
    tmpl_vars_t v;
    char tb[32], rb[32];
    static char payload[300];
    channel_vars(ctx, &v, tb, sizeof tb, rb, sizeof rb);
    tmpl_expand(mq.payload[0] ? mq.payload : "{\"status\":\"{status}\",\"device\":\"{device}\",\"time\":\"{time}\"}",
                &v, payload, sizeof payload, true);
    net_tls_opts_t o = NET_TLS_OPTS_DEFAULT;
    int64_t dl = esp_timer_get_time() + 30LL * 1000000LL;
    net_conn_t *c = tls ? net_connect_tls(host, port, &o, err, errlen) : net_connect_plain(host, port, &o, err, errlen);
    if (!c) return CR_RETRY;
    static uint8_t b[640];
    channel_result_t r = CR_RETRY;
    char id[24];
    snprintf(id, sizeof id, "%s-%04x", net_wifi_device_name(), (unsigned)(esp_random() & 0xFFFF));
    size_t n = mqtt_build_connect(b, sizeof b, id, mq.user, mq.pass, 30);
    if (!n || net_write_all(c, b, n, dl) < 0) { snprintf(err, errlen, "CONNECT gonderilemedi"); goto out; }
    memset(b, 0, sizeof b);
    int got = read_packet(c, b, sizeof b, dl);
    int ack = got > 0 ? mqtt_parse_connack(b, (size_t)got) : -1;
    if (ack != 0) {
        snprintf(err, errlen, "CONNACK %d", ack);
        r = (ack == 4 || ack == 5) ? CR_CONFIG : CR_RETRY;   /* 4/5: kullanıcı/parola ya da yetki */
        goto out;
    }
    uint16_t pid = (uint16_t)(ctx->trigger_seq ? ctx->trigger_seq : 1);
    n = mqtt_build_publish(b, sizeof b, mq.topic, (const uint8_t *)payload, strlen(payload), 1, mq.retain, pid);
    if (!n || net_write_all(c, b, n, dl) < 0) { snprintf(err, errlen, "PUBLISH gonderilemedi"); goto out; }
    got = read_packet(c, b, sizeof b, dl);
    if (got <= 0 || mqtt_parse_puback(b, (size_t)got, pid) != 0) { snprintf(err, errlen, "PUBACK yok"); goto out; }
    r = CR_OK;
    n = mqtt_build_disconnect(b, sizeof b);
    net_write_all(c, b, n, dl);
out:
    net_close(c);
    return r;
}
