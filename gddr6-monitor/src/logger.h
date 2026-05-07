#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <time.h>

struct logger;

struct log_record {
    time_t       ts;
    int          gpu_idx;
    const char  *name;

    uint32_t     vram_temp_c;

    int          has_core_temp;
    unsigned int core_temp_c;

    int          has_threshold;
    unsigned int core_threshold_c;

    int          has_clocks_util;
    unsigned int mem_clock_mhz;
    unsigned int gpu_clock_mhz;
    unsigned int util_gpu_pct;
    unsigned int util_mem_pct;
};

struct logger *logger_open(const char *path, int truncate);
void           logger_write(struct logger *lg, const struct log_record *rec);
void           logger_close(struct logger *lg);

#endif
