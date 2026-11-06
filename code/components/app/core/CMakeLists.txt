# core — uygulama çekirdeği. Durum makineleri saf C (IDF'siz, masaüstünde test edilir);
# app_core görevi (core.c) adım 2.3'te eklenir.
idf_component_register(SRCS "warn_sched.c" "timer_sm.c" "relay_sm.c" "button_sm.c" "outbox.c" "core.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cfg evlog io nvs_flash esp_timer esp_system ota)
ls_strict_warnings(${COMPONENT_LIB})
