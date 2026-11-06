/*
 * evlog_file — halka → /log/events.jsonl (sys_worker'da), döndürme, sayfalı okuma, kontrol noktası.
 * Dosya işlemleri s_flock altında: boşaltıcı (sys_worker), okuyucu (httpd), sys_restart çağıranı.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "evlog.h"
#include "evlog_priv.h"
#include "jsonl_tail.h"
#include "storage.h"
#include "sys_worker.h"

#define F_CUR   STG_LOG "/events.jsonl"
#define F_OLD   STG_LOG "/events.1.jsonl"
#define F_CKPT  STG_LOG "/checkpoint.bin"
#define ROTATE_AT (256L * 1024L)
#define CUR_OLD_BIT 0x80000000u

typedef struct { uint32_t boot, up, epoch, heap_min; } ckpt_t;

static SemaphoreHandle_t s_flock;
static atomic_bool s_flush_pending;
static esp_timer_handle_t s_ckpt_timer;

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* halkayı dosyaya boşalt; çağıran s_flock'u tutar */
static void drain_locked(void)
{
    static char line[352];
    if (!storage_acquire(STG_LOG)) return;          /* /log yok: satırlar halkada kalır */
    FILE *f = NULL;
    while (evlog_pop_line(line, sizeof line)) {
        if (!f && !(f = fopen(F_CUR, "a"))) break;
        fputs(line, f);
        fputc('\n', f);
    }
    if (f) {
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        if (file_size(F_CUR) > ROTATE_AT) {
            remove(F_OLD);
            rename(F_CUR, F_OLD);
        }
    }
    storage_release(STG_LOG);
}

static void flush_job(void *arg)
{
    (void)arg;
    atomic_store(&s_flush_pending, false);
    xSemaphoreTake(s_flock, portMAX_DELAY);
    drain_locked();
    xSemaphoreGive(s_flock);
}

static void flush_hook(void)
{
    if (!atomic_exchange(&s_flush_pending, true)) {
        if (sys_worker_post(flush_job, NULL) != ESP_OK) atomic_store(&s_flush_pending, false);
    }
}

static void restart_hook(void)
{
    if (xSemaphoreTake(s_flock, pdMS_TO_TICKS(500)) == pdTRUE) {
        drain_locked();
        xSemaphoreGive(s_flock);
    }
}

static void ckpt_job(void *arg)
{
    (void)arg;
    ckpt_t c;
    evlog_checkpoint_values(&c.boot, &c.up, &c.epoch, &c.heap_min);
    if (!storage_acquire(STG_LOG)) return;
    FILE *f = fopen(F_CKPT, "wb");
    if (f) {
        fwrite(&c, sizeof c, 1, f);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    storage_release(STG_LOG);
}

static void ckpt_timer_cb(void *arg)
{
    (void)arg;
    sys_worker_post(ckpt_job, NULL);
    /* ilk kontrol noktası 60. saniyede, sonra 10 dakikada bir (ALTE) */
    esp_timer_stop(s_ckpt_timer);
    esp_timer_start_periodic(s_ckpt_timer, 600ULL * 1000000ULL);
}

esp_err_t evlog_file_start(void)
{
    s_flock = xSemaphoreCreateMutex();
    if (!s_flock) return ESP_ERR_NO_MEM;

    ckpt_t c = { 0 };
    bool have = false;
    if (storage_acquire(STG_LOG)) {
        FILE *f = fopen(F_CKPT, "rb");
        if (f) {
            have = fread(&c, sizeof c, 1, f) == 1;
            fclose(f);
        }
        storage_release(STG_LOG);
    }
    evlog_apply_checkpoint(have, c.boot, c.up, c.epoch, c.heap_min);

    const esp_timer_create_args_t ta = { .callback = ckpt_timer_cb, .name = "evlog_ck" };
    if (esp_timer_create(&ta, &s_ckpt_timer) == ESP_OK) esp_timer_start_once(s_ckpt_timer, 60ULL * 1000000ULL);
    evlog_set_restart_hook(restart_hook);
    evlog_set_flush_hook(flush_hook);               /* halkada bekleyen varsa hemen boşaltılır */
    return ESP_OK;
}

esp_err_t evlog_read(uint32_t cursor, int limit, evlog_line_cb_t cb, void *ctx, uint32_t *next_cursor)
{
    *next_cursor = 0;
    if (!s_flock || !storage_acquire(STG_LOG)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_flock, portMAX_DELAY);
    drain_locked();                                  /* en yeni satırlar da görünsün */
    bool old = cursor & CUR_OLD_BIT;
    long end = cursor ? (long)(cursor & ~CUR_OLD_BIT) - 1 : -1;
    int left = limit;
    for (int pass = 0; pass < 2 && left > 0; pass++) {
        const char *path = old ? F_OLD : F_CUR;
        FILE *f = fopen(path, "rb");
        long next = 0;
        if (f) {
            if (end < 0) end = file_size(path);
            left -= jsonl_read_back(f, end, left, cb, ctx, &next);
            fclose(f);
        }
        if (next > 0) {
            *next_cursor = (uint32_t)(next + 1) | (old ? CUR_OLD_BIT : 0);
            break;
        }
        if (old) break;
        old = true;                                   /* güncel dosya bitti: eskiye geç */
        end = -1;
        if (left == 0 && file_size(F_OLD) > 0) *next_cursor = CUR_OLD_BIT | (uint32_t)(file_size(F_OLD) + 1);
    }
    xSemaphoreGive(s_flock);
    storage_release(STG_LOG);
    return ESP_OK;
}

esp_err_t evlog_clear(void)
{
    if (!s_flock || !storage_acquire(STG_LOG)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_flock, portMAX_DELAY);
    remove(F_CUR);
    remove(F_OLD);
    xSemaphoreGive(s_flock);
    storage_release(STG_LOG);
    evlog_write(EVL_INFO, "sys", "olay gunlugu temizlendi");
    return ESP_OK;
}
