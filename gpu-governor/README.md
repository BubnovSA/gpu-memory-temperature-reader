# GPU Governor

NVML-based daemon that keeps an NVIDIA GPU in a compact, efficient operating
point under AI/compute workloads, while protecting it from thermal overruns
and restoring factory defaults on shutdown.

Three-state machine driven by GPU utilization and temperature:

| State    | Behavior                                                  |
|----------|-----------------------------------------------------------|
| `IDLE`   | Power limit and clocks at factory defaults.               |
| `COMPUTE`| Power capped to `PL_COMPUTE`, core clock locked to a narrow range. |
| `THERMAL`| Power capped to `PL_THERMAL`, clock lock released — cool down. |

## What this actually is (and isn't)

This is **power capping + a locked GPU-clock ceiling**. When both are
enforced, the driver internally selects the lowest voltage on the V/F curve
that can sustain the clock within the power budget — i.e. an *implicit*
undervolt, not a true V/F curve edit.

On Linux, proper V/F curve editing (MSI Afterburner-style) is not exposed by
NVML and requires either an X server with Coolbits enabled, or out-of-tree
tools like [`nvidia-pstated`](https://github.com/sasha0552/nvidia-pstated).
This governor is intentionally simpler and runs on any NVML-supporting
driver.

## Requirements

- NVIDIA Linux driver (tested on 535+).
- Python 3.8+.
- `nvidia-ml-py` (`pip3 install nvidia-ml-py`).
- `systemd`.
- `root` (required to change power limit / persistence mode / clocks).

`nvmlDeviceSetGpuLockedClocks` is used for clock locking. It is supported on
Turing and newer consumer cards (RTX 20xx+, including Ampere like the 3090).
If the call is not supported, the governor logs a clear error on first use
and continues with power-cap-only mode.

## Install

```sh
cd gpu-governor
sh install.sh
sudo systemctl enable --now gpu-governor
journalctl -u gpu-governor -f
```

`install.sh` copies the daemon to `/usr/local/bin/`, the unit to
`/etc/systemd/system/`, and creates `/etc/gpu-governor/config` from the
example if it doesn't exist.

## Configure

Edit `/etc/gpu-governor/config` (loaded by systemd `EnvironmentFile=`), then
`sudo systemctl restart gpu-governor`.

| Variable                         | Default | Meaning                                                              |
|----------------------------------|---------|----------------------------------------------------------------------|
| `GPU_GOVERNOR_INDEX`             | `0`     | GPU index (`nvidia-smi -L`)                                          |
| `GPU_GOVERNOR_POLL_INTERVAL`     | `5`     | Seconds between ticks                                                |
| `GPU_GOVERNOR_UTIL_HIGH`         | `25`    | Rolling avg util ≥ this → enter `COMPUTE`                            |
| `GPU_GOVERNOR_UTIL_LOW`          | `10`    | Rolling avg util ≤ this → enter `IDLE`                               |
| `GPU_GOVERNOR_UTIL_WINDOW`       | `3`     | Rolling window size for utilization (samples)                        |
| `GPU_GOVERNOR_STATE_MIN_SECONDS` | `15`    | Min time in state before a util-driven transition                    |
| `GPU_GOVERNOR_TEMP_ENTER`        | `83`    | Enter `THERMAL` immediately at or above (°C)                         |
| `GPU_GOVERNOR_TEMP_EXIT`         | `78`    | Leave `THERMAL` only after cooling strictly below (°C) — hysteresis  |
| `GPU_GOVERNOR_PL_COMPUTE`        | `290`   | Power limit in `COMPUTE` (W)                                         |
| `GPU_GOVERNOR_PL_THERMAL`        | `160`   | Power limit in `THERMAL` (W)                                         |
| `GPU_GOVERNOR_CLOCK_MIN`         | `210`   | Locked clock lower bound (MHz)                                       |
| `GPU_GOVERNOR_CLOCK_MAX`         | `1800`  | Locked clock upper bound (MHz) — main tuning knob for efficiency     |

### Tuning notes

- **Lower `CLOCK_MAX`** for more aggressive implicit undervolt. RTX 3090
  typically stays stable at 1650–1800 MHz for mixed AI workloads with
  ~30–40 W savings vs. stock.
- **Watch the journal for `STATE` lines** while benchmarking; if the state
  is flapping between `COMPUTE` and `IDLE`, increase `UTIL_WINDOW` or
  `STATE_MIN_SECONDS`.
- **`PL_THERMAL`** is only applied when the GPU reaches `TEMP_ENTER`. Set
  it below the case-sustainable power for your cooler.

## State-machine semantics

1. **Thermal takes priority.** If temp ≥ `TEMP_ENTER`, transition to
   `THERMAL` immediately regardless of time-in-state or utilization.
2. **Thermal recovery is debounced.** Leave `THERMAL` only when
   `temp < TEMP_EXIT` *and* at least `STATE_MIN_SECONDS` have passed in the
   state. Hysteresis (`TEMP_ENTER > TEMP_EXIT`) prevents flapping.
3. **Utilization transitions** use the rolling average of the last
   `UTIL_WINDOW` samples, and only fire after `STATE_MIN_SECONDS` in the
   current state. This avoids reacting to brief spikes (e.g. a browser or
   desktop compositor).

## Differences from the naive version

This is a hardened rewrite of a simpler governor (see the conversation that
motivated this module). Fixes applied:

- **Thermal hysteresis & recovery path.** The naive version had no explicit
  exit edge from `THERMAL` — if utilization stayed in the 11–24 % "dead
  zone" it could stay throttled indefinitely. Now there is a clear
  `temp < TEMP_EXIT` + min-time exit.
- **Utilization debouncing.** Transitions use a rolling average, not a
  single sample, so a one-off 30 % spike will not yank you into `COMPUTE`.
- **`SetGpuLockedClocks` instead of `SetApplicationsClocks`.** The latter
  is often unsupported on consumer Ampere and silently becomes a no-op in
  the original script. We use the modern API and log clearly if it isn't
  available.
- **Graceful shutdown.** Signal handlers restore the factory power limit
  and unlock the clock on `SIGTERM`/`SIGINT`, so stopping the service does
  not leave the GPU in a pinned state.
- **Config via `EnvironmentFile`.** All tuning parameters are externalized
  to `/etc/gpu-governor/config`; no editing the Python source to retune.
- **Logs go to `journald`** (stderr), not an unbounded `/var/log/` file.
- **Validation of defaults read from NVML.** `PowerManagementDefaultLimit`
  is captured at startup and used to restore on `IDLE` and on shutdown.

## Troubleshooting

Check what the daemon is doing right now:

```sh
journalctl -u gpu-governor -f
```

A typical `COMPUTE` tick looks like:

```
util= 92% avg= 89.0% temp= 71°C state=COMPUTE age= 124s
```

If you see:

- `nvmlDeviceSetGpuLockedClocks not supported` — your driver or card
  doesn't expose clock locking; the governor will only do power capping.
- `Persistence mode not set` — the daemon isn't running as root. Check the
  service unit — it should be running under the default root user.
- `Power limit -> N W` followed by nothing else — NVML accepted the cap.
  Confirm with `nvidia-smi` in another terminal.
- Flapping between states — raise `UTIL_WINDOW` and/or
  `STATE_MIN_SECONDS`.

Manual verification:

```sh
nvidia-smi -q -d POWER,CLOCK,TEMPERATURE
```

Reset everything by hand if the daemon is stopped and the GPU seems stuck:

```sh
sudo nvidia-smi -rgc         # reset locked clocks
sudo nvidia-smi -rac         # reset applications clocks
sudo nvidia-smi -pl <DEFAULT_W>  # default is printed on daemon start
```

## Uninstall

```sh
sudo systemctl disable --now gpu-governor
sudo rm /etc/systemd/system/gpu-governor.service /usr/local/bin/gpu-governor.py
sudo rm -r /etc/gpu-governor
sudo systemctl daemon-reload
```

## Scope

- Single-GPU. Multi-GPU is a straightforward extension (one governor per
  handle) but not implemented here.
- No process-aware tuning (no detection of which CUDA client is running).
- No VRAM-hotspot awareness — use the sibling `gddr6` tool in this repo
  for that.
