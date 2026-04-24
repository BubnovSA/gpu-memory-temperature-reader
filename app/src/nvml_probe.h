#ifndef NVML_PROBE_H
#define NVML_PROBE_H

#include <stdint.h>

struct nvml_metrics {
    int          valid;              /* 1 if clocks+util were fetched ok */
    unsigned int mem_clock_mhz;
    unsigned int gpu_clock_mhz;
    unsigned int util_gpu_pct;
    unsigned int util_mem_pct;

    int          core_temp_valid;    /* 1 if core_temp_c was fetched ok */
    unsigned int core_temp_c;
};

int  nvml_probe_init(void);          /* 0 on success, -1 if NVML not available */
void nvml_probe_shutdown(void);

/* Resolve NVML handle by PCI address. Returns handle index (>=0) or -1. */
int  nvml_probe_attach(uint16_t domain, uint8_t bus, uint8_t dev, uint8_t func);

/* Live read: clocks, util, core temp. */
void nvml_probe_read(int handle_idx, struct nvml_metrics *out);

/* Static read once per device: returns 0 and writes slowdown threshold °C
 * into *out_c, or -1 if not available (*out_c unchanged). */
int  nvml_probe_slowdown_threshold(int handle_idx, unsigned int *out_c);

#endif
