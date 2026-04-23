#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct logger {
    FILE *f;
};

static void write_ts(FILE *f, const char *kind)
{
    char tsbuf[40];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(tsbuf, sizeof tsbuf, "%Y-%m-%dT%H:%M:%S%z", &tm);
    fprintf(f, "# === session %s %s ===\n", kind, tsbuf);
}

struct logger *logger_open(const char *path, int truncate)
{
    if (!path || !*path) return NULL;

    int fresh = truncate;
    if (!fresh) {
        FILE *probe = fopen(path, "r");
        if (!probe) {
            fresh = 1;
        } else {
            fseek(probe, 0, SEEK_END);
            if (ftell(probe) == 0) fresh = 1;
            fclose(probe);
        }
    }

    FILE *f = fopen(path, truncate ? "w" : "a");
    if (!f) return NULL;

    struct logger *lg = calloc(1, sizeof *lg);
    if (!lg) { fclose(f); return NULL; }
    lg->f = f;

    if (fresh) {
        fprintf(f, "# timestamp,gpu_idx,name,temp_c,mem_clock_mhz,gpu_clock_mhz,util_gpu_pct,util_mem_pct\n");
    }
    write_ts(f, "start");
    fflush(f);
    return lg;
}

void logger_write(struct logger *lg, const struct log_record *r)
{
    if (!lg || !lg->f) return;
    if (r->has_nvml) {
        fprintf(lg->f, "%ld,%d,%s,%u,%u,%u,%u,%u\n",
                (long)r->ts, r->gpu_idx, r->name, r->temp_c,
                r->mem_clock_mhz, r->gpu_clock_mhz,
                r->util_gpu_pct, r->util_mem_pct);
    } else {
        fprintf(lg->f, "%ld,%d,%s,%u,,,,\n",
                (long)r->ts, r->gpu_idx, r->name, r->temp_c);
    }
    fflush(lg->f);
}

void logger_close(struct logger *lg)
{
    if (!lg) return;
    if (lg->f) {
        fflush(lg->f);
        write_ts(lg->f, "end");
        fclose(lg->f);
    }
    free(lg);
}
