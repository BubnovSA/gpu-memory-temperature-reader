// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sergey Bubnov
//
// gddr6.h — public interface of libgddr6 (PCI scan + /dev/mem mmap of
// the architecture-specific VRAM-temperature MMIO register).
//
// VRAM-readout technique and the PCI device→offset table originate from
// https://github.com/olealgoritme/gddr6 (no explicit license at the
// time of forking; treated as a publicly disclosed RE result). See the
// repository LICENSE file for the full attribution note.

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
