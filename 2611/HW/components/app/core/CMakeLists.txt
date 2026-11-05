# core — uygulama çekirdeği. Durum makineleri saf C (IDF'siz, masaüstünde test edilir);
# app_core görevi (core.c) adım 2.3'te eklenir.
idf_component_register(SRCS "warn_sched.c" "timer_sm.c" "relay_sm.c" "button_sm.c" "outbox.c"
                       INCLUDE_DIRS "include")
ls_strict_warnings(${COMPONENT_LIB})
