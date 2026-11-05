/*
 * jobs — uzun süren işlerin durum kaydı (OTA, testler, kademe-2 silme...).
 * GUI POST ile işi başlatır, {job_id} alır, GET /api/jobs/<id> ile izler.
 * 8 kayıt; dolunca en eski BİTMİŞ kayıt (yoksa en eski kayıt) yeniden kullanılır.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED } job_state_t;
typedef uint16_t job_id_t;                 /* 0 = geçersiz */

typedef struct {
    job_id_t id;
    job_state_t state;
    int8_t pct;                            /* -1 = bilinmiyor */
    char kind[16];
    char msg[80];
} job_info_t;

job_id_t jobs_create(const char *kind);
void     jobs_update(job_id_t id, job_state_t st, int pct, const char *msg);
bool     jobs_get(job_id_t id, job_info_t *out);
const char *jobs_state_name(job_state_t st);
void     jobs_reset(void);                 /* yalnız testler */
