// gddr6.h
#ifndef GDDR6_H
#define GDDR6_H

#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>

#define MAX_DEVICES 32

struct device
{
    uint64_t bar0;
    uint8_t bus, dev, func;
    uint32_t offset;
    uint16_t dev_id;
    const char *vram;
    const char *arch;
    const char *name;
    void *mapped_addr;
    uint64_t phys_addr;
    uint64_t base_offset;

    /* statistics */
    uint32_t temp_min;
    uint32_t temp_max;
    uint64_t temp_sum;
    uint64_t temp_count;
};

struct gddr6_ctx {
  struct device *devices;
  int num_devices;
  int fd;
};

extern volatile sig_atomic_t gddr6_shutdown_requested;

void gddr6_init(void);
void gddr6_memory_map(void);
void gddr6_cleanup(void);
void gddr6_request_shutdown(int sig);
int gddr6_detect_compatible_gpus(void);
const struct gddr6_ctx *gddr6_get_ctx(void);

#endif // GDDR6_H
