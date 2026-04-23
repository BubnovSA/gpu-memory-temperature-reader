# gpu-governor — NVML power/clock manager with thermal protection

A `systemd`-managed daemon that keeps an NVIDIA GPU in an efficient operating
point under compute workloads, protects it from overheating, and restores
factory defaults on shutdown.

## TL;DR

Install it, enable the service, and forget about it. The daemon watches the
GPU every few seconds and switches between three states:

- **`IDLE`** — default clocks and power; suitable for desktop use and light work.
- **`COMPUTE`** — capped power (default 290 W) and a locked GPU-clock ceiling
  (default 1800 MHz). The driver internally picks the lowest voltage that can
  sustain the clock within the power budget — an *implicit undervolt*.
- **`THERMAL`** — emergency backoff at 83 °C: power dropped to 160 W and the
  clock lock released until the GPU cools back below 78 °C.

On `systemctl stop` (or reboot) the daemon first restores the factory power
limit and unlocks the clock, so you never leave the GPU in a pinned state.

## How it works — worked example

RTX 3090 on a desktop. Defaults. Timeline:

```
t=0s    state=IDLE           GPU at ~35 W at idle, no action.
t=10s   You start training.  GPU util jumps to 95 %.
t=25s   Rolling util avg ≥ 25 % for 3 samples, 15 s passed in IDLE.
          STATE IDLE -> COMPUTE
          Power limit -> 290 W
          GPU clock locked to 210–1800 MHz
t=300s  GPU pulling 285 W, running ~1770 MHz, VRAM hits 83 °C.
          STATE COMPUTE -> THERMAL
          Power limit -> 160 W
          GPU clock unlocked
t=330s  Temp dropped to 77 °C, ≥ 15 s in THERMAL.
          STATE THERMAL -> IDLE  (factory power limit restored)
t=345s  Util still high, window full, 15 s in IDLE.
          STATE IDLE -> COMPUTE  (ramps back up)
t=3600s Training ends, util drops below 10 %.
          STATE COMPUTE -> IDLE
────────────────────────────
sudo systemctl stop gpu-governor
          "Restoring GPU defaults"
          Power limit -> default
          GPU clock unlocked
          NVML shutdown
```

Watch this live:

```sh
journalctl -u gpu-governor -f
```

## Is this "undervolting"?

Not in the MSI Afterburner curve-editor sense. The governor locks the clock
ceiling and caps power; the driver picks the lowest voltage that sustains
that ceiling within the budget. On a 3090 this typically saves 30–40 W under
training workloads without needing Coolbits or out-of-tree tools.

For a true V/F curve edit on Linux, see
[nvidia-pstated](https://github.com/sasha0552/nvidia-pstated).

## Requirements

- Linux with NVIDIA proprietary driver (535+ recommended)
- `python3` (3.8+) and `pip`
- Python package: `nvidia-ml-py` (provides `pynvml`)
- `systemd`
- Root (the service runs as root; privileged NVML calls require it)
- NVIDIA GPU. Turing (RTX 20xx) or newer for clock locking; older cards
  fall back to power-cap-only mode automatically.

```sh
sudo apt install python3-pip
pip3 install nvidia-ml-py
```

## Install

```sh
cd gpu-governor
sh install.sh
sudo systemctl enable --now gpu-governor
journalctl -u gpu-governor -f
```

The installer places:

| File                                              | Purpose                                    |
|---------------------------------------------------|--------------------------------------------|
| `/usr/local/bin/gpu-governor.py`                  | the daemon                                 |
| `/etc/systemd/system/gpu-governor.service`        | the unit                                   |
| `/etc/gpu-governor/config.example`                | reference config (reinstalled every time)  |
| `/etc/gpu-governor/config`                        | active config (created from example only if absent — your edits survive updates) |

## Update (after code or config changes)

```sh
cd /path/to/repo/gpu-governor
git pull                               # or apply local changes
sudo systemctl stop gpu-governor
sh install.sh                          # refreshes daemon + unit + config.example
sudo systemctl daemon-reload
sudo systemctl start gpu-governor
```

The installer does **not** overwrite `/etc/gpu-governor/config`. If the
example file gained new variables you want to pick up:

```sh
diff /etc/gpu-governor/config /etc/gpu-governor/config.example
```

…then merge by hand and `sudo systemctl restart gpu-governor`.

## Uninstall

```sh
sudo systemctl disable --now gpu-governor
sudo rm /etc/systemd/system/gpu-governor.service
sudo rm /usr/local/bin/gpu-governor.py
sudo rm -r /etc/gpu-governor
sudo systemctl daemon-reload
```

The shutdown handler restores factory power/clock state automatically on
`stop`. If the daemon crashed and left the GPU in a pinned state, reset
manually:

```sh
sudo nvidia-smi -rgc                     # release locked clocks
sudo nvidia-smi -pl <default-wattage>    # default value is printed in the daemon's startup log
```

## Configuration

Edit `/etc/gpu-governor/config`, then:

```sh
sudo systemctl restart gpu-governor
```

All variables (defaults in parentheses):

| Variable                         | Default | Purpose                                                                                    |
|----------------------------------|---------|--------------------------------------------------------------------------------------------|
| `GPU_GOVERNOR_INDEX`             | `0`     | Which GPU (`nvidia-smi -L` shows the indices).                                             |
| `GPU_GOVERNOR_POLL_INTERVAL`     | `5`     | Seconds between ticks.                                                                     |
| `GPU_GOVERNOR_UTIL_HIGH`         | `25`    | Rolling avg util ≥ this → enter `COMPUTE`.                                                 |
| `GPU_GOVERNOR_UTIL_LOW`          | `10`    | Rolling avg util ≤ this → enter `IDLE`.                                                    |
| `GPU_GOVERNOR_UTIL_WINDOW`       | `3`     | Samples in the rolling window. Larger = slower, more stable.                               |
| `GPU_GOVERNOR_STATE_MIN_SECONDS` | `15`    | Min time in a state before a util-driven transition. Raise to damp flapping.               |
| `GPU_GOVERNOR_TEMP_ENTER`        | `83`    | °C. Enter `THERMAL` at or above.                                                           |
| `GPU_GOVERNOR_TEMP_EXIT`         | `78`    | °C. Leave `THERMAL` only strictly below. The gap is the hysteresis margin.                 |
| `GPU_GOVERNOR_PL_COMPUTE`        | `290`   | Watts. Power cap in `COMPUTE`.                                                             |
| `GPU_GOVERNOR_PL_THERMAL`        | `160`   | Watts. Power cap in `THERMAL`.                                                             |
| `GPU_GOVERNOR_CLOCK_MIN`         | `210`   | MHz. Lower bound of the locked clock range.                                                |
| `GPU_GOVERNOR_CLOCK_MAX`         | `1800`  | MHz. Upper bound. **Main efficiency knob** — lower values give a stronger implicit undervolt. |

Config is validated on startup (hysteresis gap, range ordering, positive
intervals). On invalid values the daemon exits with code 2 and the service
does **not** auto-restart — fix the file and `systemctl restart` yourself.

## Common tuning recipes

### Maximum efficiency, willing to lose some peak perf

```
GPU_GOVERNOR_CLOCK_MAX=1650
GPU_GOVERNOR_PL_COMPUTE=260
```

Typical result on 3090: ~10 % lower peak training throughput, ~40–60 W less
heat.

### Don't let the state flap during desktop use

```
GPU_GOVERNOR_UTIL_WINDOW=6
GPU_GOVERNOR_STATE_MIN_SECONDS=30
```

### Weak cooler, tighter thermal control

```
GPU_GOVERNOR_TEMP_ENTER=78
GPU_GOVERNOR_TEMP_EXIT=72
GPU_GOVERNOR_PL_THERMAL=120
```

## State-machine reference

| Priority | Condition                                                                                | Action                                        |
|----------|------------------------------------------------------------------------------------------|-----------------------------------------------|
| 1        | `temp >= TEMP_ENTER`                                                                     | → `THERMAL` (immediate; bypasses min-time)    |
| 2        | `state == THERMAL` **and** `temp < TEMP_EXIT` **and** `age >= STATE_MIN_SECONDS`         | → `IDLE`                                      |
| 3        | `state != THERMAL` **and** window full **and** `age >= STATE_MIN_SECONDS` **and** `avg ≥ UTIL_HIGH` | → `COMPUTE`                                   |
| 4        | `state != THERMAL` **and** window full **and** `age >= STATE_MIN_SECONDS` **and** `avg ≤ UTIL_LOW`  | → `IDLE`                                      |

Any other condition: stay put.

## Testing

Manual checklist to walk through on the target Linux box after install or
any change.

### 1. Service is alive

```sh
systemctl status gpu-governor                    # [1] Active: active (running)
journalctl -u gpu-governor -f                    # [2] one INFO line every POLL_INTERVAL seconds
```

Expected first lines after start:

- `Config: Config(gpu_index=0, …)`
- `NVML init OK, managing GPU0 '<name>'`
- `Default power limit: <N> W`
- `Persistence mode enabled`

### 2. Config validation rejects bad configs

```sh
# Introduce a broken hysteresis:
sudo tee -a /etc/gpu-governor/config >/dev/null <<EOF
GPU_GOVERNOR_TEMP_EXIT=85
GPU_GOVERNOR_TEMP_ENTER=83
EOF
sudo systemctl restart gpu-governor
systemctl status gpu-governor                    # [3] "(code=exited, status=2)", NOT restarting
journalctl -u gpu-governor | tail -3             # [4] "Config error: TEMP_EXIT must be < TEMP_ENTER"

# Revert:
sudo sed -i '/^GPU_GOVERNOR_TEMP_\(EXIT\|ENTER\)=8[35]$/d' /etc/gpu-governor/config
sudo systemctl restart gpu-governor              # now starts cleanly
```

### 3. IDLE → COMPUTE → IDLE under load

```sh
# Terminal A:
journalctl -u gpu-governor -f

# Terminal B (start a GPU load, e.g.):
python3 -c 'import torch; a=torch.randn(10000,10000,device="cuda"); [(a@a).sum().item() for _ in range(30)]'
```

Expected in terminal A:

```
STATE IDLE -> COMPUTE
Power limit -> 290 W
GPU clock locked to 210–1800 MHz
```

Verify externally while the load runs:

```sh
nvidia-smi --query-gpu=power.limit,clocks.gr --format=csv   # [5] shows 290 W / clock near ceiling
```

When the load finishes, within ~15–30 s:

```
STATE COMPUTE -> IDLE
Power limit -> default (<N> W)
GPU clock unlocked                              # [6]
```

### 4. Thermal path (controlled test)

Lower the thresholds temporarily so the transition triggers quickly:

```sh
sudo tee -a /etc/gpu-governor/config >/dev/null <<EOF
GPU_GOVERNOR_TEMP_ENTER=60
GPU_GOVERNOR_TEMP_EXIT=55
EOF
sudo systemctl restart gpu-governor
```

Start a short GPU load and watch the journal:

```
STATE COMPUTE -> THERMAL                        # [7] triggered at ≥ 60 °C
Power limit -> 160 W
GPU clock unlocked
```

Stop the load and wait:

```
STATE THERMAL -> IDLE                           # [8] only once temp < 55 °C AND ≥ 15 s in THERMAL
```

Revert:

```sh
sudo sed -i '/^GPU_GOVERNOR_TEMP_\(ENTER\|EXIT\)=\(60\|55\)$/d' /etc/gpu-governor/config
sudo systemctl restart gpu-governor
```

### 5. Graceful shutdown restores defaults

```sh
# While a COMPUTE cap is applied:
nvidia-smi --query-gpu=power.limit,clocks.gr --format=csv   # capture "capped" state
sudo systemctl stop gpu-governor                             # [9]
nvidia-smi --query-gpu=power.limit,clocks.gr --format=csv   # [10] back to factory power, clocks free
journalctl -u gpu-governor | tail -5                         # [11] "Restoring GPU defaults", "NVML shutdown"
```

### 6. Update workflow

```sh
# Simulate an update:
sudo systemctl stop gpu-governor
cd /path/to/repo/gpu-governor
sh install.sh                                                # [12] reinstalls without touching /etc/gpu-governor/config
cat /etc/gpu-governor/config                                 # [13] your customizations preserved
sudo systemctl start gpu-governor
```

### 7. Fallback when clock locking is unsupported

If your driver or card lacks `nvmlDeviceSetGpuLockedClocks`, the first
transition to `COMPUTE` logs:

```
nvmlDeviceSetGpuLockedClocks not supported (...); continuing with power-cap only
```

Power capping still applies. This is expected, not a failure.

## Troubleshooting

- **"Persistence mode not set"** — daemon is not root. `systemctl cat
  gpu-governor` should show no `User=` override.
- **No state transitions visible under load** — workload might not be
  hitting the managed GPU, or utilization is in the 11–24 % dead zone.
  Cross-check with `nvidia-smi dmon` in parallel.
- **Manual `nvidia-smi -pl <N>` conflicts with the governor** — expected.
  The governor actively re-applies its targets on every tick. Stop the
  service if you want to override manually.
- **Service won't start after edit** — run `systemctl status gpu-governor`
  and `journalctl -u gpu-governor | tail`; you'll see either a config
  validation error (exit 2) or a Python traceback.

## Scope and non-goals

- **Single GPU.** Multi-GPU requires multiple service instances; a systemd
  template unit is not included yet.
- **No process awareness** — the governor doesn't know which CUDA client
  is running.
- **No VRAM hotspot tracking** — use the sibling [`gddr6`](../app/README.md)
  tool in this repo for that.
- **Not a replacement for `nvidia-pstated`** or other V/F curve editors.
