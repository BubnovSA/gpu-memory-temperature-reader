#include "sparkline.h"
#include <string.h>

static const char *const BLOCKS[8] = {
    "\xe2\x96\x81", /* U+2581 ▁ */
    "\xe2\x96\x82", /* U+2582 ▂ */
    "\xe2\x96\x83", /* U+2583 ▃ */
    "\xe2\x96\x84", /* U+2584 ▄ */
    "\xe2\x96\x85", /* U+2585 ▅ */
    "\xe2\x96\x86", /* U+2586 ▆ */
    "\xe2\x96\x87", /* U+2587 ▇ */
    "\xe2\x96\x88", /* U+2588 █ */
};

void spark_init(struct spark *s)
{
    s->head = 0;
    s->count = 0;
}

void spark_push(struct spark *s, uint32_t v)
{
    s->buf[s->head] = v;
    s->head = (s->head + 1) % SPARK_CAP;
    if (s->count < SPARK_CAP) s->count++;
}

int spark_render(const struct spark *s, int width, char *out, size_t out_cap)
{
    if (out_cap == 0) return 0;
    out[0] = '\0';
    if (s->count == 0) return 0;

    if (width > s->count) width = s->count;
    if (width > SPARK_CAP) width = SPARK_CAP;
    if (width <= 0) return 0;

    int start = (s->head - width + SPARK_CAP) % SPARK_CAP;

    uint32_t mn = UINT32_MAX, mx = 0;
    for (int i = 0; i < width; i++) {
        uint32_t v = s->buf[(start + i) % SPARK_CAP];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    uint32_t range = (mx > mn) ? (mx - mn) : 1;

    size_t used = 0;
    for (int i = 0; i < width; i++) {
        uint32_t v = s->buf[(start + i) % SPARK_CAP];
        unsigned idx = (unsigned)(((uint64_t)(v - mn) * 7) / range);
        if (idx > 7) idx = 7;
        if (used + 3 + 1 > out_cap) break;
        memcpy(out + used, BLOCKS[idx], 3);
        used += 3;
    }
    out[used] = '\0';
    return (int)used;
}
