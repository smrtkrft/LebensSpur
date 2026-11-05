#include "button_sm.h"

button_action_t button_sm_press(button_sm_t *b, int64_t now)
{
    if (b->count == 0 || now - b->first_us > (int64_t)BUTTON_SPAN_MS * 1000) {
        b->count = 1;
        b->first_us = now;
        return BA_NONE;
    }
    if (++b->count >= 5) {
        b->count = 0;
        return BA_FACTORY1;
    }
    return BA_NONE;
}

button_action_t button_sm_window_end(button_sm_t *b)
{
    uint8_t c = b->count;
    b->count = 0;
    if (c == 0) return BA_NONE;
    return c >= 3 ? BA_RESTART : BA_CHECKIN;
}
