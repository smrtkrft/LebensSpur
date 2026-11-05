/*
 * delivery — teslimat görevi (öncelik 5, 10 KB). app_core'dan bir anda TEK iş alır (seri: bir anda tek TLS),
 * kanalı çalıştırır, sonucu EV_DELIVERY_RESULT olarak döner. Test işlerinde jobs kaydını günceller ve
 * testi geçen, hiç açılmamış kanalı otomatik açar (karar 8).
 */
#pragma once
#include "esp_err.h"

esp_err_t delivery_start(void);       /* core_set_dispatch'i de kurar */
