# gddr6 — VRAM temperature & GPU metrics monitor

Real-time CLI monitor for NVIDIA GPUs. Reads **GDDR6/GDDR6X VRAM temperature**
directly from the GPU's MMIO register (reverse-engineered from the Linux
driver), optionally decorates the output with **GPU/memory clocks and
utilization** via NVML, and writes samples to a **CSV log file**.

See the [root README](../README.md) for the list of supported GPUs and
shared kernel/Secure-Boot prerequisites.

## Requirements

- `libpci-dev` (for PCI enumeration)
- `cmake` + `build-essential`
- NVIDIA proprietary driver (NVML is bundled with it; optional but recommended)

```sh
sudo apt install libpci-dev cmake build-essential -y
```

## Install

```sh
cd /path/to/repo
./build_install.sh
# answer "y" at the prompt to install into /usr/local
```

This produces `build/bin/gddr6` and, on install, places `gddr6` in
`/usr/local/bin/` and `libgddr6.a` in `/usr/local/lib/`.

`gddr6` always needs root (opens `/dev/mem`):

```sh
sudo gddr6
```

## Update (rebuild after code changes)

```sh
cd /path/to/repo
git pull                       # or apply local edits
rm -rf build                   # clean previous artifacts
./build_install.sh             # answer "y" to overwrite the /usr/local installs
```

No caching outside `build/` — a fresh rebuild is always safe.

## Uninstall

```sh
sudo rm /usr/local/bin/gddr6
sudo rm /usr/local/lib/libgddr6.a
rm -rf build                   # optional: also drop local build artifacts
```

## Usage

```
Usage: gddr6 [options]
  -i, --interval <s>   Polling interval in seconds (default: 1)
  -n, --count <n>      Exit after N readings (0 = infinite)
  -l, --log <path>     CSV log file path (default: ./gddr6.log)
      --no-log         Disable CSV logging
      --truncate       Clear log file on start
  -h, --help           Print this help and exit
```

### Terminal output

Three lines per GPU, redrawn in place every interval:

```
GPU0 RTX 3090              Gclk 1770MHz  Mclk  9751MHz  util  92%/ 68%
  Core   cur  65°C  min  52  max  71   [  70% of 92°C ]
  VRAM   cur  72°C  min  58  max  78
```

- First line: GPU name + (if NVML loaded) core/memory clock and GPU/memory
  utilization.
- **Core** row: current temperature from NVML + cumulative min/max
  since the process started. If NVML exposes the slowdown threshold, the
  bracket shows `current / threshold * 100%` — i.e. how close you are to
  the point where the driver starts throttling.
- **VRAM** row: cumulative min/max from the MMIO-read VRAM sensor.

When stdout is a TTY, values are colored:
- `min` — always green (low watermark)
- `max` — always red (high watermark)
- `cur` — gradient by temperature:
  - **Core**: ≤65 °C green, 66–72 °C yellow, ≥73 °C red
  - **VRAM**: ≤82 °C green, 83–86 °C yellow, ≥87 °C red

Colors are suppressed when stdout is piped or redirected.
- If NVML is unavailable the Core row shows `— (needs NVML)` and the first
  line omits the clock/util suffix; VRAM still works on its own.

When stdout is not a TTY (piped, redirected, `tee`), ANSI cursor controls
are suppressed — output scrolls line by line.

### CSV log format

Default path: `./gddr6.log` (relative to the working directory). Columns
(10):

```
# timestamp,gpu_idx,name,vram_temp_c,core_temp_c,core_threshold_c,mem_clock_mhz,gpu_clock_mhz,util_gpu_pct,util_mem_pct
# === session start 2026-04-24T14:30:00+0300 ===
1713962200,0,RTX 3090,72,65,92,9751,1770,92,68
...
# === session end 2026-04-24T14:31:02+0300 ===
```

- Header is written only to a new/empty file.
- Each invocation appends one `session start`/`session end` marker pair;
  pass `--truncate` to clear first.
- Lines starting with `#` are comments — most CSV parsers either skip them
  or are configurable to.
- Rows always have 10 columns. Fields without a value are left blank
  (e.g. `...,72,,,,,,` when NVML is unavailable).

## Testing

Manual checklist — walk through top to bottom on the target machine.

### Smoke test

```sh
sudo ./build/bin/gddr6 -n 3
```
Expected: three 3-line blocks per GPU (header + Core + VRAM), then a
`Core / VRAM` summary table, exit 0. Startup should print
`NVML: clocks/util/core-temp enabled for N of N GPU(s)` — if it's
`NVML unavailable`, the tool still works but the Core row shows
`— (needs NVML)` and the first line omits clock/util info.

### Interactive display

```sh
sudo ./build/bin/gddr6
```
Expected: each GPU occupies exactly 3 rewriting-in-place lines. Ctrl+C
triggers the final summary and clean exit. The `[ NN% of M°C ]` bracket
on the Core row reflects current core temp vs. the driver's slowdown
threshold (typically 92 °C on GA102 like the RTX 3090).

### Logging

```sh
sudo ./build/bin/gddr6 -n 3
cat ./gddr6.log
```
Expected: CSV header, `# === session start ... ===`, three rows per GPU,
`# === session end ... ===`.

```sh
sudo ./build/bin/gddr6 -n 3 --truncate && wc -l ./gddr6.log
```
Expected: file is reset, only the fresh session remains.

```sh
sudo ./build/bin/gddr6 --no-log -n 2 && ls -la ./gddr6.log
```
Expected: log file is not modified.

### Live tail

```sh
sudo ./build/bin/gddr6 -l /tmp/x.log &
tail -f /tmp/x.log
# Ctrl+C on tail, then `sudo kill %1` to stop gddr6
```
Expected: new CSV lines appear in `tail -f` immediately (each write is
flushed).

## Known limitations

- **VRAM temperature** is supported only for the GPUs listed in the
  [root README](../README.md#gddr6-vram-readout). Unlisted cards will cause
  *No compatible GPU found*.
- **NVML handle lookup** uses PCI domain 0. On multi-socket servers with
  non-zero PCI domains the temperature still works, but clock/util columns
  may not populate.
- **CSV log has no rotation** — long unattended runs grow the file
  unbounded. Rotate externally if needed (`logrotate`, `savelog`).
