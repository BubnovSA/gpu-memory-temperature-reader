// app.c
#include "gddr6.h"
#include "sparkline.h"
#include "nvml_probe.h"
#include "logger.h"
#include "telegram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <stdint.h>

#define SPARK_WIDTH_DEFAULT 40

struct app_config {
    int          interval_s;
    int          max_readings;
    int          json_output;
    int          enable_log;
    int          truncate_log;
    int          enable_graph;
    int          spark_width;
    const char  *log_path;

    struct telegram_cfg tg;
    unsigned int alert_temp_c;
    int          alert_cooldown_s;
};

struct per_device {
    struct spark spark;
    int          nvml_idx;
    struct nvml_metrics metrics;
    time_t       last_alert_ts;
};

static void register_signal_handlers(void)
{
    struct sigaction sa;
    sa.sa_handler = gddr6_request_shutdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Auto-reap children fired by telegram_send(). */
    signal(SIGCHLD, SIG_IGN);
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -i, --interval <s>         Polling interval in seconds (default: 1)\n"
        "  -n, --count <n>            Exit after N readings (0 = infinite)\n"
        "  -j, --json                 NDJSON output on stdout (disables TUI)\n"
        "  -l, --log <path>           CSV log file path (default: ./gddr6.log)\n"
        "      --no-log               Disable CSV logging\n"
        "      --truncate             Clear log file on start (default: append with session marker)\n"
        "      --no-graph             Disable in-terminal sparkline\n"
        "      --history <n>          Sparkline width in columns (default: %d, max %d)\n"
        "      --telegram-token <t>   Telegram bot token for alerts\n"
        "      --telegram-chat <id>   Telegram chat id for alerts\n"
        "      --alert-temp <c>       Alert when VRAM temp >= <c> °C (0 = disabled)\n"
        "      --alert-cooldown <s>   Min seconds between alerts per GPU (default: 300)\n"
        "  -h, --help                 Print this help and exit\n",
        prog, SPARK_WIDTH_DEFAULT, SPARK_CAP);
}

static void print_json_line(const struct gddr6_ctx *ctx,
                            const uint32_t *temps,
                            const struct per_device *pd)
{
    printf("{\"ts\":%ld,\"gpus\":[", (long)time(NULL));
    for (int i = 0; i < ctx->num_devices; i++) {
        const struct device *d = &ctx->devices[i];
        const struct nvml_metrics *m = &pd[i].metrics;
        if (i > 0) printf(",");
        printf("{\"name\":\"%s\",\"arch\":\"%s\",\"vram\":\"%s\",\"temp_c\":%u",
               d->name, d->arch, d->vram, temps[i]);
        if (m->valid) {
            printf(",\"mem_clock_mhz\":%u,\"gpu_clock_mhz\":%u,"
                   "\"util_gpu_pct\":%u,\"util_mem_pct\":%u",
                   m->mem_clock_mhz, m->gpu_clock_mhz,
                   m->util_gpu_pct, m->util_mem_pct);
        }
        printf("}");
    }
    printf("]}\n");
    fflush(stdout);
}

static void print_statistics(const struct gddr6_ctx *ctx)
{
    printf("\n--- VRAM Temperature Statistics ---\n");
    printf("%-28s %8s %8s %8s\n", "GPU", "Min °C", "Max °C", "Avg °C");
    printf("%-28s %8s %8s %8s\n", "---", "------", "------", "------");
    for (int i = 0; i < ctx->num_devices; i++) {
        const struct device *d = &ctx->devices[i];
        if (d->temp_count == 0) {
            printf("%-28s %8s %8s %8s\n", d->name, "n/a", "n/a", "n/a");
        } else {
            uint32_t avg = (uint32_t)(d->temp_sum / d->temp_count);
            printf("%-28s %8u %8u %8u\n", d->name, d->temp_min, d->temp_max, avg);
        }
    }
}

static void print_statistics_json(const struct gddr6_ctx *ctx)
{
    fprintf(stderr, "{\"event\":\"summary\",\"gpus\":[");
    for (int i = 0; i < ctx->num_devices; i++) {
        const struct device *d = &ctx->devices[i];
        if (i > 0) fprintf(stderr, ",");
        if (d->temp_count == 0) {
            fprintf(stderr, "{\"name\":\"%s\",\"min_c\":null,\"max_c\":null,\"avg_c\":null}",
                    d->name);
        } else {
            uint32_t avg = (uint32_t)(d->temp_sum / d->temp_count);
            fprintf(stderr, "{\"name\":\"%s\",\"min_c\":%u,\"max_c\":%u,\"avg_c\":%u}",
                    d->name, d->temp_min, d->temp_max, avg);
        }
    }
    fprintf(stderr, "]}\n");
}

static void render_device_line(int idx, const struct device *d,
                               uint32_t cur_temp,
                               const struct nvml_metrics *m,
                               int use_ansi)
{
    uint32_t mn  = d->temp_count ? d->temp_min : cur_temp;
    uint32_t mx  = d->temp_count ? d->temp_max : cur_temp;
    uint32_t avg = d->temp_count ? (uint32_t)(d->temp_sum / d->temp_count) : cur_temp;

    printf("GPU%d %-20s T %3u°C  min %3u  max %3u  avg %3u",
           idx, d->name, cur_temp, mn, mx, avg);
    if (m->valid) {
        printf("  Mclk %5uMHz  Gclk %4uMHz  util %3u%%/%3u%%",
               m->mem_clock_mhz, m->gpu_clock_mhz,
               m->util_gpu_pct, m->util_mem_pct);
    }
    if (use_ansi) printf("\033[K");
    printf("\n");
}

static void render_spark_line(const struct spark *sp, int width, int use_ansi)
{
    char graph[SPARK_CAP * 3 + 1];
    spark_render(sp, width, graph, sizeof graph);
    printf("     %s", graph);
    if (use_ansi) printf("\033[K");
    printf("\n");
}

static void monitor_loop(const struct app_config *cfg,
                         struct gddr6_ctx *ctx,
                         struct per_device *pd,
                         struct logger *lg)
{
    int readings = 0;
    int use_ansi = !cfg->json_output && isatty(STDOUT_FILENO);
    int lines_per_device = 1 + (cfg->enable_graph ? 1 : 0);
    int rendered = 0;

    while (!gddr6_shutdown_requested) {
        if (cfg->max_readings > 0 && readings >= cfg->max_readings)
            break;

        uint32_t temps[MAX_DEVICES];
        time_t now = time(NULL);

        for (int i = 0; i < ctx->num_devices; i++) {
            struct device *d = &ctx->devices[i];
            if (d->mapped_addr == NULL || d->mapped_addr == MAP_FAILED) {
                temps[i] = 0;
                continue;
            }
            void *virt = (uint8_t *)d->mapped_addr + (d->phys_addr - d->base_offset);
            uint32_t raw  = *((uint32_t *)virt);
            uint32_t temp = (raw & 0x00000fff) / 0x20;
            temps[i] = temp;

            if (temp < d->temp_min) d->temp_min = temp;
            if (temp > d->temp_max) d->temp_max = temp;
            d->temp_sum   += temp;
            d->temp_count += 1;

            spark_push(&pd[i].spark, temp);
            nvml_probe_read(pd[i].nvml_idx, &pd[i].metrics);
        }

        if (cfg->json_output) {
            print_json_line(ctx, temps, pd);
        } else {
            if (rendered && use_ansi) {
                printf("\033[%dA", lines_per_device * ctx->num_devices);
            }
            for (int i = 0; i < ctx->num_devices; i++) {
                render_device_line(i, &ctx->devices[i], temps[i],
                                   &pd[i].metrics, use_ansi);
                if (cfg->enable_graph)
                    render_spark_line(&pd[i].spark, cfg->spark_width, use_ansi);
            }
            fflush(stdout);
            rendered = 1;
        }

        if (lg) {
            for (int i = 0; i < ctx->num_devices; i++) {
                struct log_record rec = {
                    .ts             = now,
                    .gpu_idx        = i,
                    .name           = ctx->devices[i].name,
                    .temp_c         = temps[i],
                    .has_nvml       = pd[i].metrics.valid,
                    .mem_clock_mhz  = pd[i].metrics.mem_clock_mhz,
                    .gpu_clock_mhz  = pd[i].metrics.gpu_clock_mhz,
                    .util_gpu_pct   = pd[i].metrics.util_gpu_pct,
                    .util_mem_pct   = pd[i].metrics.util_mem_pct,
                };
                logger_write(lg, &rec);
            }
        }

        if (cfg->alert_temp_c > 0 && telegram_enabled(&cfg->tg)) {
            for (int i = 0; i < ctx->num_devices; i++) {
                if (temps[i] < cfg->alert_temp_c) continue;
                if (now - pd[i].last_alert_ts < cfg->alert_cooldown_s) continue;
                char host[128] = {0};
                gethostname(host, sizeof host - 1);
                char msg[384];
                snprintf(msg, sizeof msg,
                         "[gddr6] %s: VRAM %u°C >= threshold %u°C (host: %s)",
                         ctx->devices[i].name, temps[i],
                         cfg->alert_temp_c, host);
                telegram_send(&cfg->tg, msg);
                pd[i].last_alert_ts = now;
            }
        }

        readings++;
        sleep((unsigned)cfg->interval_s);
    }
}

enum {
    OPT_NO_LOG = 1000,
    OPT_TRUNCATE,
    OPT_NO_GRAPH,
    OPT_HISTORY,
    OPT_TG_TOKEN,
    OPT_TG_CHAT,
    OPT_ALERT_TEMP,
    OPT_ALERT_COOLDOWN,
};

int main(int argc, char **argv)
{
    struct app_config cfg = {
        .interval_s       = 1,
        .max_readings     = 0,
        .json_output      = 0,
        .enable_log       = 1,
        .truncate_log     = 0,
        .enable_graph     = 1,
        .spark_width      = SPARK_WIDTH_DEFAULT,
        .log_path         = "./gddr6.log",
        .tg               = { NULL, NULL },
        .alert_temp_c     = 0,
        .alert_cooldown_s = 300,
    };

    static struct option long_opts[] = {
        {"interval",        required_argument, 0, 'i'},
        {"count",           required_argument, 0, 'n'},
        {"json",            no_argument,       0, 'j'},
        {"log",             required_argument, 0, 'l'},
        {"no-log",          no_argument,       0, OPT_NO_LOG},
        {"truncate",        no_argument,       0, OPT_TRUNCATE},
        {"no-graph",        no_argument,       0, OPT_NO_GRAPH},
        {"history",         required_argument, 0, OPT_HISTORY},
        {"telegram-token",  required_argument, 0, OPT_TG_TOKEN},
        {"telegram-chat",   required_argument, 0, OPT_TG_CHAT},
        {"alert-temp",      required_argument, 0, OPT_ALERT_TEMP},
        {"alert-cooldown",  required_argument, 0, OPT_ALERT_COOLDOWN},
        {"help",            no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:n:jl:h", long_opts, NULL)) != -1) {
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
        case 'j':
            cfg.json_output = 1;
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
        case OPT_NO_GRAPH:
            cfg.enable_graph = 0;
            break;
        case OPT_HISTORY:
            v = strtol(optarg, &end, 10);
            if (*end == '\0' && v >= 4 && v <= SPARK_CAP)
                cfg.spark_width = (int)v;
            break;
        case OPT_TG_TOKEN:
            cfg.tg.token = optarg;
            break;
        case OPT_TG_CHAT:
            cfg.tg.chat_id = optarg;
            break;
        case OPT_ALERT_TEMP:
            v = strtol(optarg, &end, 10);
            if (*end == '\0' && v >= 0 && v <= 150)
                cfg.alert_temp_c = (unsigned int)v;
            break;
        case OPT_ALERT_COOLDOWN:
            v = strtol(optarg, &end, 10);
            if (*end == '\0' && v >= 0) cfg.alert_cooldown_s = (int)v;
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
        spark_init(&pd[i].spark);
        pd[i].nvml_idx = -1;
    }

    int nvml_ok = (nvml_probe_init() == 0);
    if (nvml_ok) {
        int attached = 0;
        for (int i = 0; i < ctx->num_devices; i++) {
            /* PCI domain not tracked by libgddr6 — use 0 (near-universal). */
            pd[i].nvml_idx = nvml_probe_attach(
                0, ctx->devices[i].bus, ctx->devices[i].dev, ctx->devices[i].func);
            if (pd[i].nvml_idx >= 0) attached++;
        }
        if (attached == 0) {
            fprintf(stderr, "NVML loaded but no GPU handles matched by PCI id.\n");
        } else {
            printf("NVML: clocks/util enabled for %d of %d GPU(s)\n",
                   attached, ctx->num_devices);
        }
    } else {
        fprintf(stderr, "NVML unavailable — clocks/utilization disabled.\n");
    }

    struct logger *lg = NULL;
    if (cfg.enable_log && !cfg.json_output) {
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

    if (cfg.json_output)
        print_statistics_json(ctx);
    else
        print_statistics(ctx);

    logger_close(lg);
    if (nvml_ok) nvml_probe_shutdown();
    free(pd);
    gddr6_cleanup();
    return 0;
}
