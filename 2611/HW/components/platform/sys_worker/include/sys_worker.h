/*
 * sys_worker — sistem altyapısı işçisi (öncelik 3, 12 KB yığın, TWDT abonesi).
 * Uzun ve bloklayan işler burada: OTA, dosya yazımı (günlük boşaltma, yedek), kademe-2 silme.
 * Ağ mesajı GÖNDERMEZ (o delivery görevinin işi).
 */
#pragma once
#include "esp_err.h"

typedef void (*sys_job_fn_t)(void *arg);

esp_err_t sys_worker_start(void);
/* İş kuyruğa girer (8 derin, 0 bekleme). Dolu → ESP_ERR_NO_MEM (çağıran günlüğe yazar). */
esp_err_t sys_worker_post(sys_job_fn_t fn, void *arg);
/* Uzun işler (OTA parçası, silme bloğu) arada çağırır: görev bekçisini besler. */
void sys_worker_feed(void);
