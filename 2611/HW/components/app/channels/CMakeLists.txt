# channels — dört teslimat kanalı. smtp_proto/mqtt_pkt/tmpl saf C (masaüstü testli).
idf_component_register(SRCS "smtp_proto.c" "mqtt_pkt.c" "tmpl.c" "channel.c" "ch_smtp.c" "ch_http.c" "ch_mqtt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES core
                       PRIV_REQUIRES cfg evlog net_tls net_wifi storage esp_http_client esp-tls mbedtls esp_timer json)
ls_strict_warnings(${COMPONENT_LIB})
