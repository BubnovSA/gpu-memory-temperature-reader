#ifndef NVML_PROBE_H
#define NVML_PROBE_H

#include <stdint.h>

struct nvml_metrics {
    int valid;
    unsigned int mem_clock_mhz;
    unsigned int gpu_clock_mhz;
    unsigned int util_gpu_pct;
    unsigned int util_mem_pct;
};

int  nvml_probe_init(void);          /* 0 on success, -1 if NVML not available */
void nvml_probe_shutdown(void);

/* Resolve NVML handle by PCI address. Returns handle index (>=0) or -1. */
int  nvml_probe_attach(uint16_t domain, uint8_t bus, uint8_t dev, uint8_t func);

void nvml_probe_read(int handle_idx, struct nvml_metrics *out);

#endif
