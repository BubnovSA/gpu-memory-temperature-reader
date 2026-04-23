# gddr6 — VRAM temperature & GPU metrics monitor

Real-time CLI monitor for NVIDIA GPUs. Reads **GDDR6/GDDR6X VRAM temperature**
directly from the GPU's MMIO register (reverse-engineered from the Linux
driver), and optionally decorates it with **core clock, memory clock, GPU
utilization, memory utilization** via NVML. Writes samples to a **CSV log
file**, draws a **sparkline graph** in the terminal, and can send **Telegram
alerts** on thermal thresholds.

See the [root README](../README.md) for the list of supported GPUs and
shared kernel/Secure-Boot prerequisites.

## Requirements

- `libpci-dev` (for PCI enumeration)
- `cmake` + `build-essential`
- NVIDIA proprietary driver (NVML is bundled with it; optional but recommended)
- `curl` on `PATH` (optional; only for Telegram alerts)

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

There is no caching/state outside `build/` — a clean rebuild is always safe
and takes a few seconds.

## Uninstall

```sh
sudo rm /usr/local/bin/gddr6
sudo rm /usr/local/lib/libgddr6.a
rm -rf build                   # optional: also drop local build artifacts
```

## Usage

```
Usage: gddr6 [options]
  -i, --interval <s>         Polling interval in seconds (default: 1)
  -n, --count <n>            Exit after N readings (0 = infinite)
  -j, --json                 NDJSON output on stdout (disables TUI)
  -l, --log <path>           CSV log file path (default: ./gddr6.log)
      --no-log               Disable CSV logging
      --truncate             Clear log file on start
      --no-graph             Disable in-terminal sparkline
      --history <n>          Sparkline width in columns (default: 40, max: 120)
      --telegram-token <t>   Telegram bot token for alerts
      --telegram-chat <id>   Telegram chat id for alerts
      --alert-temp <c>       Alert when VRAM temp >= <c> °C (0 = disabled)
      --alert-cooldown <s>   Min seconds between alerts per GPU (default: 300)
  -h, --help                 Print this help and exit
```

### Terminal output

Two-line compact block per GPU, redrawn in place every interval:

```
GPU0 RTX 4090           T  65°C  min  52  max  71  avg  63  Mclk 10502MHz  Gclk 2745MHz  util  85%/ 72%
     ▁▂▃▄▅▆▇█▇▆▅▃▂▁
```

- `min / max / avg` are cumulative since the process started.
- `Mclk / Gclk / util X%/Y%` appear only when NVML is loaded and the GPU
  handle matched (gated message is printed on startup).
- The sparkline shows the last 40 samples by default (tune with
  `--history`), auto-scaled between that window's min and max.

When stdout is not a TTY (piped, redirected, `tee`), ANSI cursor controls
are suppressed — output scrolls line by line.

### CSV log format

Default path: `./gddr6.log` (relative to the working directory). Columns:

```
# timestamp,gpu_idx,name,temp_c,mem_clock_mhz,gpu_clock_mhz,util_gpu_pct,util_mem_pct
# === session start 2026-04-24T14:30:00+0300 ===
1713962200,0,RTX 4090,65,10502,2745,85,72
...
# === session end 2026-04-24T14:31:02+0300 ===
```

- Header is written only to a new/empty file.
- Each invocation appends one `session start`/`session end` marker pair;
  pass `--truncate` to clear first.
- Lines starting with `#` are comments — most CSV parsers either skip them
  or are configurable to.
- Rows have a stable 8-column shape even when NVML metrics are unavailable
  (the NVML columns are left blank).

### JSON mode

`-j` emits NDJSON on stdout (one object per sample) and a summary object on
stderr on exit. Intended for piping:

```sh
sudo gddr6 -j | jq '.gpus[] | select(.temp_c > 70)'
```

### Telegram alerts

Create a bot via `@BotFather`, send it any message, fetch your chat id:

```sh
curl -s "https://api.telegram.org/bot<TOKEN>/getUpdates" \
  | jq '.result[].message.chat.id'
```

Run with alerts armed:

```sh
sudo gddr6 \
  --telegram-token 123456:ABC-DEF... \
  --telegram-chat 987654321 \
  --alert-temp 90 \
  --alert-cooldown 300
```

Alerts are dispatched via a forked `curl` process; network failures do not
block the main loop. `--alert-cooldown` debounces per-GPU (default 300 s).

## Testing

Manual checklist. Run top to bottom on the target machine after first
install or any non-trivial change.

### 1. Baseline

```sh
sudo gddr6 -h                                    # [1] prints usage with all flags
sudo gddr6 -n 3                                  # [2] 3 samples → summary table → exit 0
sudo gddr6 -j -n 3 | jq .                        # [3] 3 NDJSON lines, all valid
sudo gddr6 -j -n 3 2>&1 >/dev/null | jq .        # [4] summary object on stderr
```

### 2. Terminal display

```sh
sudo gddr6                                       # [5] live 2-line layout; Ctrl+C → summary
sudo gddr6 --no-graph                            # [6] single-line layout, no sparkline
sudo gddr6 --history 20                          # [7] narrower graph
sudo gddr6 | cat                                 # [8] no ANSI sequences when piped
```

### 3. NVML (clocks + utilization)

```sh
sudo gddr6 -n 3                                  # [9] startup prints "NVML: clocks/util enabled for N of N"
# In another terminal run a GPU load:
python3 -c 'import torch; a=torch.randn(10000,10000,device="cuda"); (a@a).sum().item()'
# Back to the gddr6 window — Gclk and util should change.  [10]
```

Fallback path: if startup prints `NVML unavailable`, the tool still works
but without the clock/util columns. This is expected on systems without
NVML.

### 4. Logging

```sh
sudo gddr6 -n 3 && cat ./gddr6.log                       # [11] header + session + 3 rows + session end
sudo gddr6 -n 3 && cat ./gddr6.log                       # [12] second session appended, no duplicate header
sudo gddr6 -n 2 --truncate && wc -l ./gddr6.log          # [13] file reset, small line count
sudo gddr6 --no-log -n 2 && ls -la ./gddr6.log           # [14] log untouched
sudo gddr6 -l /tmp/custom.log -n 2 && cat /tmp/custom.log # [15] custom path honored

# Live tail (should show rows appearing immediately, thanks to per-write fflush):
sudo gddr6 -l /tmp/x.log &                                # [16]
tail -f /tmp/x.log
# ...Ctrl+C both
```

### 5. Telegram

Preconditions: a bot token and chat id.

```sh
# Force-trigger by setting threshold below current temp:
sudo gddr6 \
  --telegram-token <TOKEN> --telegram-chat <CHAT_ID> \
  --alert-temp 40 --alert-cooldown 10
# [17] First message in chat within 1 polling interval
# [18] Re-fires every ~10 s while condition holds
# [19] Ctrl+C to stop
```

### 6. Shutdown

```sh
sudo gddr6                                       # [20] Ctrl+C → summary + "session end" in log
sudo gddr6 & sleep 3; sudo kill -TERM $!         # [21] same graceful behavior on SIGTERM
sudo gddr6 & sleep 3; sudo kill -9 $!            # [22] summary skipped (expected)
```

## Known limitations

- **VRAM temperature** is supported only for the GPUs listed in the
  [root README](../README.md#gddr6-vram-readout). For unlisted cards the
  utility simply exits with *No compatible GPU found*.
- **NVML handle lookup** uses PCI domain 0. On multi-socket servers with
  non-zero PCI domains, temperature still works but clocks/util columns may
  not populate.
- **CSV log has no rotation.** Long unattended runs grow the file without
  bound. Rotate externally if needed (`logrotate`, `savelog`).
