#include "nvml_probe.h"
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

typedef void *nvmlDevice_t;
typedef int   nvmlReturn_t;
#define NVML_SUCCESS 0

typedef enum {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM       = 1,
    NVML_CLOCK_MEM      = 2,
} nvmlClockType_t;

typedef enum {
    NVML_TEMPERATURE_GPU = 0,
} nvmlTemperatureSensors_t;

typedef enum {
    NVML_TEMPERATURE_THRESHOLD_SHUTDOWN = 0,
    NVML_TEMPERATURE_THRESHOLD_SLOWDOWN = 1,
} nvmlTemperatureThresholds_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

static void *dl = NULL;
static nvmlReturn_t (*p_init)(void);
static nvmlReturn_t (*p_shutdown)(void);
static nvmlReturn_t (*p_handle_by_pci)(const char *, nvmlDevice_t *);
static nvmlReturn_t (*p_clock)(nvmlDevice_t, nvmlClockType_t, unsigned int *);
static nvmlReturn_t (*p_util)(nvmlDevice_t, nvmlUtilization_t *);
static nvmlReturn_t (*p_temp)(nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int *);
static nvmlReturn_t (*p_temp_threshold)(nvmlDevice_t, nvmlTemperatureThresholds_t, unsigned int *);

#define NVML_MAX_HANDLES 32
static nvmlDevice_t handles[NVML_MAX_HANDLES];
static int n_handles = 0;

int nvml_probe_init(void)
{
    dl = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!dl) dl = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
    if (!dl) return -1;

    p_init = dlsym(dl, "nvmlInit_v2");
    if (!p_init) p_init = dlsym(dl, "nvmlInit");

    p_shutdown       = dlsym(dl, "nvmlShutdown");
    p_handle_by_pci  = dlsym(dl, "nvmlDeviceGetHandleByPciBusId_v2");
    if (!p_handle_by_pci)
        p_handle_by_pci = dlsym(dl, "nvmlDeviceGetHandleByPciBusId");
    p_clock          = dlsym(dl, "nvmlDeviceGetClockInfo");
    p_util           = dlsym(dl, "nvmlDeviceGetUtilizationRates");
    p_temp           = dlsym(dl, "nvmlDeviceGetTemperature");
    p_temp_threshold = dlsym(dl, "nvmlDeviceGetTemperatureThreshold");

    if (!p_init || !p_shutdown || !p_handle_by_pci || !p_clock || !p_util || !p_temp) {
        dlclose(dl);
        dl = NULL;
        return -1;
    }
    /* p_temp_threshold is optional — feature still works without it */

    if (p_init() != NVML_SUCCESS) {
        dlclose(dl);
        dl = NULL;
        return -1;
    }
    n_handles = 0;
    return 0;
}

void nvml_probe_shutdown(void)
{
    if (!dl) return;
    if (p_shutdown) p_shutdown();
    dlclose(dl);
    dl = NULL;
    n_handles = 0;
}

int nvml_probe_attach(uint16_t domain, uint8_t bus, uint8_t dev, uint8_t func)
{
    if (!dl || n_handles >= NVML_MAX_HANDLES) return -1;

    char pciid[32];
    snprintf(pciid, sizeof pciid, "%04x:%02x:%02x.%x", domain, bus, dev, func);

    nvmlDevice_t h;
    if (p_handle_by_pci(pciid, &h) != NVML_SUCCESS)
        return -1;

    handles[n_handles] = h;
    return n_handles++;
}

void nvml_probe_read(int idx, struct nvml_metrics *out)
{
    memset(out, 0, sizeof *out);
    if (!dl || idx < 0 || idx >= n_handles) return;

    unsigned int clk;
    nvmlUtilization_t u;
    int clocks_util_ok = 1;

    if (p_clock(handles[idx], NVML_CLOCK_MEM, &clk) == NVML_SUCCESS)
        out->mem_clock_mhz = clk;
    else clocks_util_ok = 0;

    if (p_clock(handles[idx], NVML_CLOCK_GRAPHICS, &clk) == NVML_SUCCESS)
        out->gpu_clock_mhz = clk;
    else clocks_util_ok = 0;

    if (p_util(handles[idx], &u) == NVML_SUCCESS) {
        out->util_gpu_pct = u.gpu;
        out->util_mem_pct = u.memory;
    } else {
        clocks_util_ok = 0;
    }
    out->valid = clocks_util_ok;

    unsigned int t;
    if (p_temp(handles[idx], NVML_TEMPERATURE_GPU, &t) == NVML_SUCCESS) {
        out->core_temp_c = t;
        out->core_temp_valid = 1;
    }
}

int nvml_probe_slowdown_threshold(int idx, unsigned int *out_c)
{
    if (!dl || !p_temp_threshold || idx < 0 || idx >= n_handles) return -1;

    unsigned int t;
    if (p_temp_threshold(handles[idx], NVML_TEMPERATURE_THRESHOLD_SLOWDOWN, &t) != NVML_SUCCESS)
        return -1;
    *out_c = t;
    return 0;
}
