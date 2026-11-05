/* evlog.c <-> evlog_file.c iç arayüzü */
#pragma once
#include <stdbool.h>
#include <stdint.h>
void evlog_apply_checkpoint(bool have, uint32_t ck_boot, uint32_t ck_up, uint32_t ck_epoch, uint32_t ck_heap_min);
void evlog_checkpoint_values(uint32_t *boot, uint32_t *up, uint32_t *epoch, uint32_t *heap_min);
typedef void (*evlog_restart_hook_t)(void);
void evlog_set_restart_hook(evlog_restart_hook_t h);   /* sys_restart öncesi senkron boşaltma */
