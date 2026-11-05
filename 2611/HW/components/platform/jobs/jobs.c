/* jobs — sabit 8 kayıtlık iş defteri. Kilit: cihazda portMUX, masaüstünde yok. */
#include <stdio.h>
#include <string.h>
#include "jobs.h"

#ifdef LS_HOST_TEST
#define LOCK()
#define UNLOCK()
#else
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)
#endif

#define SLOTS 8
static job_info_t s_jobs[SLOTS];
static uint32_t s_age[SLOTS];
static uint32_t s_clock;
static job_id_t s_next = 1;

static job_info_t *find(job_id_t id)
{
    for (int i = 0; i < SLOTS; i++) {
        if (id && s_jobs[i].id == id) return &s_jobs[i];
    }
    return NULL;
}

job_id_t jobs_create(const char *kind)
{
    LOCK();
    int pick = -1;
    for (int i = 0; i < SLOTS && pick < 0; i++) {
        if (s_jobs[i].id == 0) pick = i;
    }
    for (int pass = 0; pass < 2 && pick < 0; pass++) {
        uint32_t best = UINT32_MAX;
        for (int i = 0; i < SLOTS; i++) {
            bool finished = s_jobs[i].state == JOB_DONE || s_jobs[i].state == JOB_FAILED;
            if ((pass == 1 || finished) && s_age[i] < best) { best = s_age[i]; pick = i; }
        }
    }
    job_id_t id = s_next++;
    if (s_next == 0) s_next = 1;
    s_jobs[pick] = (job_info_t){ .id = id, .state = JOB_QUEUED, .pct = -1 };
    snprintf(s_jobs[pick].kind, sizeof s_jobs[pick].kind, "%s", kind);
    s_age[pick] = ++s_clock;
    UNLOCK();
    return id;
}

void jobs_update(job_id_t id, job_state_t st, int pct, const char *msg)
{
    LOCK();
    job_info_t *j = find(id);
    if (j) {
        j->state = st;
        j->pct = (int8_t)(pct < -1 ? -1 : pct > 100 ? 100 : pct);
        if (msg) snprintf(j->msg, sizeof j->msg, "%s", msg);
    }
    UNLOCK();
}

bool jobs_get(job_id_t id, job_info_t *out)
{
    LOCK();
    job_info_t *j = find(id);
    if (j) *out = *j;
    UNLOCK();
    return j != NULL;
}

const char *jobs_state_name(job_state_t st)
{
    static const char *const n[] = { "queued", "running", "done", "failed" };
    return (unsigned)st < 4 ? n[st] : "failed";
}

void jobs_reset(void)
{
    memset(s_jobs, 0, sizeof s_jobs);
    memset(s_age, 0, sizeof s_age);
    s_clock = 0;
    s_next = 1;
}
