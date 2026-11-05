/*
 * button_sm — fiziksel düğme çoklu basma (ALTE ile aynı anlamlar, saf C):
 *   1–2 basma → erteleme · 3–4 → yeniden başlatma · 5 → fabrika sıfırlama kademe 1 (anında).
 * Basmalar arası en çok 400 ms beklenir (pencere app_core'da tek atımlık zamanlayıcı);
 * dizinin tamamı 3 sn içinde olmalı.
 */
#pragma once
#include <stdint.h>

#define BUTTON_WINDOW_MS 400
#define BUTTON_SPAN_MS   3000

typedef enum { BA_NONE, BA_CHECKIN, BA_RESTART, BA_FACTORY1 } button_action_t;
typedef struct { uint8_t count; int64_t first_us; } button_sm_t;

button_action_t button_sm_press(button_sm_t *b, int64_t now_us);   /* 5. basmada BA_FACTORY1 */
button_action_t button_sm_window_end(button_sm_t *b);              /* 400 ms sessizlikten sonra */
