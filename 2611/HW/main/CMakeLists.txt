idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_system esp_app_format heap http cfg evlog storage esp_timer sys_worker core io net_wifi net_tls api json delivery ota espcoredump)
ls_strict_warnings(${COMPONENT_LIB})

# Geliştirme kancası: tezgâh cihazına WiFi'yi bir kez yazmak için (yalnız derleme ortamından;
# kaynakta ve git'te parola yok). Kayıttan sonra değişkensiz yeniden derlenmeli.
if(DEFINED ENV{LS_DEV_WIFI_SSID})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        LS_DEV_WIFI_SSID="$ENV{LS_DEV_WIFI_SSID}" LS_DEV_WIFI_PASS="$ENV{LS_DEV_WIFI_PASS}")
endif()
