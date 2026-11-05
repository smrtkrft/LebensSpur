#include <string.h>
#include "jsonl_tail.h"

#define CHUNK 512
#define LMAX  512

int jsonl_read_back(FILE *f, long end, int limit, jsonl_line_cb_t cb, void *ctx, long *next_end)
{
    static char buf[CHUNK];
    static char line[LMAX + 1];
    size_t llen = 0;                 /* line[] sonuna doğru biriken (ters sırada) */
    bool overflow = false;
    int given = 0;
    long pos = end;
    long line_start_end = end;       /* yarım satırın bittiği konum (devam imleci için) */

    while (pos > 0 && given < limit) {
        long n = pos >= CHUNK ? CHUNK : pos;
        pos -= n;
        if (fseek(f, pos, SEEK_SET) != 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) break;
        for (long i = n - 1; i >= 0; i--) {
            char c = buf[i];
            if (c == '\n') {
                if (llen && !overflow) {
                    line[LMAX] = 0;
                    if (!cb(line + LMAX - llen, ctx)) { *next_end = pos + i; return given + 1; }
                    given++;
                }
                llen = 0;
                overflow = false;
                line_start_end = pos + i;
                if (given >= limit) { *next_end = line_start_end; return given; }
                continue;
            }
            if (llen < LMAX) line[LMAX - 1 - llen++] = c;
            else overflow = true;
        }
    }
    if (pos == 0 && given < limit && llen && !overflow) {   /* dosyanın ilk satırı */
        line[LMAX] = 0;
        cb(line + LMAX - llen, ctx);
        given++;
        line_start_end = 0;
    }
    *next_end = (pos == 0 && (given < limit || line_start_end == 0)) ? 0 : line_start_end;
    return given;
}
