/* mqtt_pkt — MQTT 3.1.1 paket kurucu/çözücü (saf C). Tek yayın: CONNECT → PUBLISH(QoS1) → DISCONNECT. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t mqtt_build_connect(uint8_t *buf, size_t cap, const char *client_id, const char *user, const char *pass,
                          uint16_t keepalive_s);
size_t mqtt_build_publish(uint8_t *buf, size_t cap, const char *topic, const uint8_t *payload, size_t len,
                          uint8_t qos, bool retain, uint16_t pkt_id);
size_t mqtt_build_disconnect(uint8_t *buf, size_t cap);
/* CONNACK: 0 kabul, 1..5 broker ret kodu, -1 bozuk */
int mqtt_parse_connack(const uint8_t *buf, size_t len);
/* PUBACK: paket kimliği eşleşirse 0, değilse -1 */
int mqtt_parse_puback(const uint8_t *buf, size_t len, uint16_t pkt_id);
/* sabit başlıktan sonra kalan uzunluk; *hdr = sabit başlık boyu. Eksikse -1. */
int mqtt_remaining_len(const uint8_t *buf, size_t len, size_t *hdr);
