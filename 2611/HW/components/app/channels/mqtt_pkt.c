#include <string.h>
#include "mqtt_pkt.h"

static size_t put_varint(uint8_t *b, size_t v)
{
    size_t n = 0;
    do {
        uint8_t d = v % 128;
        v /= 128;
        b[n++] = (uint8_t)(d | (v ? 0x80 : 0));
    } while (v && n < 4);
    return n;
}

static size_t put_str(uint8_t *b, const char *s)
{
    size_t l = strlen(s);
    b[0] = (uint8_t)(l >> 8);
    b[1] = (uint8_t)l;
    memcpy(b + 2, s, l);
    return l + 2;
}

static size_t frame(uint8_t *buf, size_t cap, uint8_t type, const uint8_t *body, size_t blen)
{
    uint8_t vl[4];
    size_t n = put_varint(vl, blen);
    if (1 + n + blen > cap) return 0;
    buf[0] = type;
    memcpy(buf + 1, vl, n);
    memmove(buf + 1 + n, body, blen);
    return 1 + n + blen;
}

size_t mqtt_build_connect(uint8_t *buf, size_t cap, const char *id, const char *user, const char *pass,
                          uint16_t ka)
{
    uint8_t b[512];
    bool u = user && *user, p = u && pass && *pass;
    size_t need = 10 + 2 + strlen(id) + (u ? 2 + strlen(user) : 0) + (p ? 2 + strlen(pass) : 0);
    if (need > sizeof b) return 0;
    size_t o = put_str(b, "MQTT");
    b[o++] = 4;                                              /* 3.1.1 */
    b[o++] = (uint8_t)(0x02 | (u ? 0x80 : 0) | (p ? 0x40 : 0));   /* temiz oturum */
    b[o++] = (uint8_t)(ka >> 8);
    b[o++] = (uint8_t)ka;
    o += put_str(b + o, id);
    if (u) o += put_str(b + o, user);
    if (p) o += put_str(b + o, pass);
    size_t r = frame(buf, cap, 0x10, b, o);
    memset(b, 0, sizeof b);
    return r;
}

size_t mqtt_build_publish(uint8_t *buf, size_t cap, const char *topic, const uint8_t *pl, size_t len,
                          uint8_t qos, bool retain, uint16_t id)
{
    size_t tl = strlen(topic), body = 2 + tl + (qos ? 2 : 0) + len;
    uint8_t vl[4];
    size_t n = put_varint(vl, body);
    if (1 + n + body > cap) return 0;
    size_t o = 0;
    buf[o++] = (uint8_t)(0x30 | (qos << 1) | (retain ? 1 : 0));
    memcpy(buf + o, vl, n);
    o += n;
    o += put_str(buf + o, topic);
    if (qos) { buf[o++] = (uint8_t)(id >> 8); buf[o++] = (uint8_t)id; }
    memcpy(buf + o, pl, len);
    return o + len;
}

size_t mqtt_build_disconnect(uint8_t *buf, size_t cap)
{
    if (cap < 2) return 0;
    buf[0] = 0xE0;
    buf[1] = 0;
    return 2;
}

int mqtt_remaining_len(const uint8_t *b, size_t len, size_t *hdr)
{
    size_t v = 0, mul = 1;
    for (size_t i = 1; i < len && i <= 4; i++) {
        v += (b[i] & 127) * mul;
        mul *= 128;
        if (!(b[i] & 0x80)) { *hdr = i + 1; return (int)v; }
    }
    return -1;
}

int mqtt_parse_connack(const uint8_t *b, size_t len)
{
    if (len < 4 || b[0] != 0x20 || b[1] != 2) return -1;
    return b[3];
}

int mqtt_parse_puback(const uint8_t *b, size_t len, uint16_t id)
{
    if (len < 4 || b[0] != 0x40 || b[1] != 2) return -1;
    return ((uint16_t)(b[2] << 8 | b[3])) == id ? 0 : -1;
}
