// app.c
#include "gddr6.h"
#include "nvml_probe.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <stdint.h>

#define LINES_PER_GPU 3   /* header + Core + VRAM */

struct app_config {
    int         interval_s;
    int         max_readings;
    int         enable_log;
    int         truncate_log;
    const char *log_path;
};

struct temp_stats {
    uint32_t min;
    uint32_t max;
};

struct per_device {
    int                 nvml_idx;
    struct nvml_metrics metrics;
    unsigned int        core_threshold_c;   /* 0 if unknown */
    struct temp_stats   core;
    struct temp_stats   vram;
};

static void stats_init(struct temp_stats *s)
{
    s->min = UINT32_MAX;
    s->max = 0;
}

static void stats_update(struct temp_stats *s, uint32_t v)
{
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
}

static int stats_has_data(const struct temp_stats *s)
{
    return s->min != UINT32_MAX;
}

/* ANSI color helpers — return escape code or empty string. */
static const char *ansi_reset(int use_ansi) { return use_ansi ? "\033[0m" : ""; }

static const char *core_cur_color(uint32_t t, int use_ansi)
{
    if (!use_ansi) return "";
    if (t <= 65) return "\033[32m";   /* green  */
    if (t <= 72) return "\033[33m";   /* yellow */
    return "\033[31m";                /* red    */
}

static const char *vram_cur_color(uint32_t t, int use_ansi)
{
    if (!use_ansi) return "";
    if (t <= 82) return "\033[32m";
    if (t <= 86) return "\033[33m";
    return "\033[31m";
}

static void register_signal_handlers(void)
{
    struct sigaction sa;
    sa.sa_handler = gddr6_request_shutdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -i, --interval <s>   Polling interval in seconds (default: 1)\n"
        "  -n, --count <n>      Exit after N readings (0 = infinite)\n"
        "  -l, --log <path>     CSV log file path (default: ./gddr6.log)\n"
        "      --no-log         Disable CSV logging\n"
        "      --truncate       Clear log file on start\n"
        "  -h, --help           Print this help and exit\n",
        prog);
}

static void print_final_summary(const struct gddr6_ctx *ctx,
                                const struct per_device *pd)
{
    printf("\n--- Temperature Summary (Min / Max °C) ---\n");
    for (int i = 0; i < ctx->num_devices; i++) {
        const struct device *d = &ctx->devices[i];
        const struct per_device *p = &pd[i];
        printf("GPU%d %s\n", i, d->name);
        if (stats_has_data(&p->core))
            printf("  Core   %3u / %3u\n", p->core.min, p->core.max);
        else
            printf("  Core   —\n");
        if (stats_has_data(&p->vram))
            printf("  VRAM   %3u / %3u\n", p->vram.min, p->vram.max);
        else
            printf("  VRAM   —\n");
    }
}

static void render_header(int idx, const struct device *d,
                          const struct nvml_metrics *m, int use_ansi)
{
    printf("GPU%d %-20s", idx, d->name);
    if (m->valid) {
        printf("  Gclk %4uMHz  Mclk %5uMHz  util %3u%%/%3u%%",
               m->gpu_clock_mhz, m->mem_clock_mhz,
               m->util_gpu_pct, m->util_mem_pct);
    }
    if (use_ansi) printf("\033[K");
    printf("\n");
}

static void render_core(const struct per_device *p, int use_ansi)
{
    printf("  Core   ");
    if (p->metrics.core_temp_valid) {
        uint32_t cur = p->metrics.core_temp_c;
        uint32_t mn  = stats_has_data(&p->core) ? p->core.min : cur;
        uint32_t mx  = stats_has_data(&p->core) ? p->core.max : cur;
        printf("cur %s%3u°C%s  min %3u  max %3u",
               core_cur_color(cur, use_ansi), cur, ansi_reset(use_ansi),
               mn, mx);
        if (p->core_threshold_c > 0) {
            unsigned int pct = (cur * 100u) / p->core_threshold_c;
            printf("   [ %3u%% of %u°C ]", pct, p->core_threshold_c);
        }
    } else {
        printf("—   (needs NVML)");
    }
    if (use_ansi) printf("\033[K");
    printf("\n");
}

static void render_vram(const struct per_device *p, uint32_t cur, int use_ansi)
{
    uint32_t mn = stats_has_data(&p->vram) ? p->vram.min : cur;
    uint32_t mx = stats_has_data(&p->vram) ? p->vram.max : cur;
    printf("  VRAM   cur %s%3u°C%s  min %3u  max %3u",
           vram_cur_color(cur, use_ansi), cur, ansi_reset(use_ansi),
           mn, mx);
    if (use_ansi) printf("\033[K");
    printf("\n");
}

static void monitor_loop(const struct app_config *cfg,
                         struct gddr6_ctx *ctx,
                         struct per_device *pd,
                         struct logger *lg)
{
    int readings = 0;
    int use_ansi = isatty(STDOUT_FILENO);
    int rendered = 0;

    while (!gddr6_shutdown_requested) {
        if (cfg->max_readings > 0 && readings >= cfg->max_readings)
            break;

        uint32_t vram_temps[MAX_DEVICES];
        time_t   now = time(NULL);

        for (int i = 0; i < ctx->num_devices; i++) {
            struct device *d = &ctx->devices[i];
            if (d->mapped_addr == NULL || d->mapped_addr == MAP_FAILED) {
                vram_temps[i] = 0;
                continue;
            }
            void *virt = (uint8_t *)d->mapped_addr + (d->phys_addr - d->base_offset);
            uint32_t raw  = *((uint32_t *)virt);
            uint32_t temp = (raw & 0x00000fff) / 0x20;
            vram_temps[i] = temp;
            stats_update(&pd[i].vram, temp);

            nvml_probe_read(pd[i].nvml_idx, &pd[i].metrics);
            if (pd[i].metrics.core_temp_valid)
                stats_update(&pd[i].core, pd[i].metrics.core_temp_c);
        }

        if (rendered && use_ansi)
            printf("\033[%dA", LINES_PER_GPU * ctx->num_devices);
        for (int i = 0; i < ctx->num_devices; i++) {
            render_header(i, &ctx->devices[i], &pd[i].metrics, use_ansi);
            render_core(&pd[i], use_ansi);
            render_vram(&pd[i], vram_temps[i], use_ansi);
        }
        fflush(stdout);
        rendered = 1;

        if (lg) {
            for (int i = 0; i < ctx->num_devices; i++) {
                struct log_record rec = {
                    .ts                = now,
                    .gpu_idx           = i,
                    .name              = ctx->devices[i].name,
                    .vram_temp_c       = vram_temps[i],
                    .has_core_temp     = pd[i].metrics.core_temp_valid,
                    .core_temp_c       = pd[i].metrics.core_temp_c,
                    .has_threshold     = pd[i].core_threshold_c > 0,
                    .core_threshold_c  = pd[i].core_threshold_c,
                    .has_clocks_util   = pd[i].metrics.valid,
                    .mem_clock_mhz     = pd[i].metrics.mem_clock_mhz,
                    .gpu_clock_mhz     = pd[i].metrics.gpu_clock_mhz,
                    .util_gpu_pct      = pd[i].metrics.util_gpu_pct,
                    .util_mem_pct      = pd[i].metrics.util_mem_pct,
                };
                logger_write(lg, &rec);
            }
        }

        readings++;
        sleep((unsigned)cfg->interval_s);
    }
}

enum {
    OPT_NO_LOG = 1000,
    OPT_TRUNCATE,
};

int main(int argc, char **argv)
{
    struct app_config cfg = {
        .interval_s   = 1,
        .max_readings = 0,
        .enable_log   = 1,
        .truncate_log = 0,
        .log_path     = "./gddr6.log",
    };

    static struct option long_opts[] = {
        {"interval", required_argument, 0, 'i'},
        {"count",    required_argument, 0, 'n'},
        {"log",      required_argument, 0, 'l'},
        {"no-log",   no_argument,       0, OPT_NO_LOG},
        {"truncate", no_argument,       0, OPT_TRUNCATE},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:n:l:h", long_opts, NULL)) != -1) {
        char *end;
        long v;
        switch (opt) {
        case 'i':
            v = strtol(optarg, &end, 10);
            cfg.interval_s = (*end == '\0' && v >= 1) ? (int)v : 1;
            break;
        case 'n':
            v = strtol(optarg, &end, 10);
            cfg.max_readings = (*end == '\0' && v >= 0) ? (int)v : 0;
            break;
        case 'l':
            cfg.log_path = optarg;
            cfg.enable_log = 1;
            break;
        case OPT_NO_LOG:
            cfg.enable_log = 0;
            break;
        case OPT_TRUNCATE:
            cfg.truncate_log = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    register_signal_handlers();
    gddr6_init();

    int num_devs = gddr6_detect_compatible_gpus();
    if (num_devs == 0) {
        printf("No compatible GPU found.\n");
        return 1;
    }

    gddr6_memory_map();
    struct gddr6_ctx *ctx = (struct gddr6_ctx *)gddr6_get_ctx();

    struct per_device *pd = calloc((size_t)ctx->num_devices, sizeof *pd);
    if (!pd) {
        fprintf(stderr, "Out of memory\n");
        gddr6_cleanup();
        return 1;
    }
    for (int i = 0; i < ctx->num_devices; i++) {
        pd[i].nvml_idx = -1;
        stats_init(&pd[i].core);
        stats_init(&pd[i].vram);
    }

    int nvml_ok = (nvml_probe_init() == 0);
    if (nvml_ok) {
        int attached = 0;
        for (int i = 0; i < ctx->num_devices; i++) {
            pd[i].nvml_idx = nvml_probe_attach(
                0, ctx->devices[i].bus, ctx->devices[i].dev, ctx->devices[i].func);
            if (pd[i].nvml_idx >= 0) {
                attached++;
                unsigned int thr;
                if (nvml_probe_slowdown_threshold(pd[i].nvml_idx, &thr) == 0)
                    pd[i].core_threshold_c = thr;
            }
        }
        if (attached == 0) {
            fprintf(stderr, "NVML loaded but no GPU handles matched by PCI id.\n");
        } else {
            printf("NVML: clocks/util/core-temp enabled for %d of %d GPU(s)\n",
                   attached, ctx->num_devices);
        }
    } else {
        fprintf(stderr, "NVML unavailable — clocks/util/core-temp disabled.\n");
    }

    struct logger *lg = NULL;
    if (cfg.enable_log) {
        lg = logger_open(cfg.log_path, cfg.truncate_log);
        if (!lg) {
            fprintf(stderr, "Warning: could not open log '%s' — logging disabled.\n",
                    cfg.log_path);
        } else {
            printf("Logging to %s (%s)\n",
                   cfg.log_path, cfg.truncate_log ? "truncated" : "appending");
        }
    }

    monitor_loop(&cfg, ctx, pd, lg);

    print_final_summary(ctx, pd);

    logger_close(lg);
    if (nvml_ok) nvml_probe_shutdown();
    free(pd);
    gddr6_cleanup();
    return 0;
}
