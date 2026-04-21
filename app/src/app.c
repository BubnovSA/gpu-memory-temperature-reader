// app.c
#include "gddr6.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <stdint.h>

struct app_config {
    int interval_s;
    int max_readings;
    int json_output;
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
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-i seconds] [-n count] [-j] [-h]\n"
        "  -i <seconds>  Polling interval (default: 1)\n"
        "  -n <count>    Exit after N readings (0 = infinite, default: 0)\n"
        "  -j            JSON output (NDJSON, one object per line on stdout)\n"
        "  -h            Print this help and exit\n",
        prog);
}

static void print_json_line(const struct gddr6_ctx *ctx, const uint32_t *temps)
{
    printf("{\"ts\":%ld,\"gpus\":[", (long)time(NULL));
    for (int i = 0; i < ctx->num_devices; i++) {
        const struct device *d = &ctx->devices[i];
        if (i > 0) printf(",");
        printf("{\"name\":\"%s\",\"arch\":\"%s\",\"vram\":\"%s\",\"temp_c\":%u}",
               d->name, d->arch, d->vram, temps[i]);
    }
    printf("]}\n");
    fflush(stdout);
}

static void print_statistics(const struct gddr6_ctx *ctx)
{
    printf("\n\n--- VRAM Temperature Statistics ---\n");
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

static void monitor_loop(const struct app_config *cfg, struct gddr6_ctx *ctx)
{
    int readings = 0;

    while (!gddr6_shutdown_requested) {
        if (cfg->max_readings > 0 && readings >= cfg->max_readings)
            break;

        uint32_t temps[MAX_DEVICES];

        if (!cfg->json_output)
            printf("\rVRAM Temps: |");

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

            if (!cfg->json_output)
                printf(" %3u°C |", temp);
        }

        if (cfg->json_output)
            print_json_line(ctx, temps);
        else
            fflush(stdout);

        readings++;
        sleep((unsigned)cfg->interval_s);
    }
}

int main(int argc, char **argv)
{
    struct app_config cfg = { .interval_s = 1, .max_readings = 0, .json_output = 0 };
    int opt;

    while ((opt = getopt(argc, argv, "i:n:jh")) != -1) {
        switch (opt) {
        case 'i': {
            char *end;
            long v = strtol(optarg, &end, 10);
            cfg.interval_s = (*end == '\0' && v >= 1) ? (int)v : 1;
            break;
        }
        case 'n': {
            char *end;
            long v = strtol(optarg, &end, 10);
            cfg.max_readings = (*end == '\0' && v >= 0) ? (int)v : 0;
            break;
        }
        case 'j':
            cfg.json_output = 1;
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
    monitor_loop(&cfg, ctx);

    if (cfg.json_output)
        print_statistics_json(ctx);
    else
        print_statistics(ctx);

    gddr6_cleanup();
    return 0;
}
