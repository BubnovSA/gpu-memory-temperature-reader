#ifndef SPARKLINE_H
#define SPARKLINE_H

#include <stddef.h>
#include <stdint.h>

#define SPARK_CAP 120

struct spark {
    uint32_t buf[SPARK_CAP];
    int head;
    int count;
};

void spark_init(struct spark *s);
void spark_push(struct spark *s, uint32_t v);

/* Render last `width` samples as UTF-8 block chars into `out`.
 * out_cap must be >= 3*width + 1. Returns bytes written (excl. null). */
int spark_render(const struct spark *s, int width, char *out, size_t out_cap);

#endif
