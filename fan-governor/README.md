# fan-governor — temperature-driven fan curve daemon

A `systemd`-managed daemon that drives NVIDIA GPU fans (via NVML) and
motherboard PWM channels (via hwmon sysfs) from per-group fan curves.
On shutdown, all controlled fans are returned to firmware-default
control.

Sibling to [`gpu-governor`](../gpu-governor/) (clocks/power) and
[`gddr6`](../gddr6-monitor/) (real-time temperature monitor).

## TL;DR

Each fan **group** has:

- a temperature **source**: `gpu_core`, `gpu_vram`, or `gpu_max`
  (the hotter of the two — useful for GDDR6X, where memory often
  runs ahead of the core);
- a list of **targets**: `nvml:<gpu>:<fan|*>` for GPU fans,
  `hwmon:<chip>:pwmN` for motherboard PWM channels;
- a **curve** of `[temp_c, percent]` points (linear interpolation
  between, clamped beyond the endpoints);
- an optional **min_pwm** floor — many fans don't restart cleanly
  from a stop, so this keeps them spinning.

Above `emergency_temp` (default 88 °C) the group is forced to 100 %
until the source temperature falls below `emergency_exit`
(default 82 °C). On `systemctl stop` (or reboot) every controlled
fan is returned to its firmware policy.

## Requirements

- Linux + NVIDIA proprietary driver **≥ 525** (verified on 580.x).
  `nvmlDeviceSetFanSpeed_v2` is required.
- `python3` (3.8+) and `pynvml` (`nvidia-ml-py` binding).
- `systemd`.
- Root.
- For motherboard fans: a Linux hwmon driver exposing PWM
  (`/sys/class/hwmon/*/pwmN`). On Gigabyte H470 AORUS PRO AX you need
  the out-of-tree **`it87` DKMS** — see below.

### Installing `pynvml`

Same recipe as in [`gpu-governor`](../gpu-governor/README.md#installing-the-python-nvml-binding):

```sh
sudo apt install -y python3-pynvml
# or, if PEP 668 blocks the apt path on Ubuntu 24.04+ / Debian 12+:
sudo pip3 install nvidia-ml-py --break-system-packages
python3 -c "import pynvml; print('OK')"
```

### Enabling motherboard fan control (Gigabyte H470 AORUS PRO AX → IT8688E)

The kernel-tree `it87` driver doesn't recognize IT8688E. Use the
maintained out-of-tree fork.

```sh
sudo apt install -y dkms git build-essential linux-headers-$(uname -r)
git clone https://github.com/frankcrawford/it87.git
cd it87
sudo make dkms
```

Force the chip ID and ignore the ACPI resource conflict (Gigabyte
boards usually need both):

```sh
sudo tee /etc/modprobe.d/it87.conf >/dev/null <<'EOF'
options it87 force_id=0x8688 ignore_resource_conflict=1
EOF

echo it87 | sudo tee /etc/modules-load.d/it87.conf
sudo modprobe it87 force_id=0x8688 ignore_resource_conflict=1

sensors | head    # should now list "it8688-isa-XXXX" with pwm1..pwmN
```

**Secure Boot:** if `modprobe` says *"Required key not available"*,
either disable Secure Boot in firmware or sign the module — see the
`it87` repo README for signing.

#### Identifying which `pwmN` is `SYS_FAN3`

Stop the daemon if running, then ramp each channel and listen for the
fan speeding up:

```sh
sudo systemctl stop fan-governor 2>/dev/null || true

CHIP=$(grep -l '^it8688$' /sys/class/hwmon/*/name | xargs -n1 dirname)
echo "chip dir: $CHIP"

# Take pwm3 manual and step:
echo 1   | sudo tee "$CHIP/pwm3_enable"
echo 64  | sudo tee "$CHIP/pwm3"        # ~25 %
echo 200 | sudo tee "$CHIP/pwm3"        # ~78 %

# Restore:
echo 2 | sudo tee "$CHIP/pwm3_enable"
```

If the wrong fan moved, repeat with `pwm1`, `pwm2`, `pwm4`, `pwm5`.
The hwmon `pwmN` numbering is independent of the silkscreen on the
board, so empirical mapping is the only reliable method.

The chip's hwmon name is **the exact value** of
`cat /sys/class/hwmon/*/name` (typically `it8688`). Use that string
in your config target, e.g. `hwmon:it8688:pwm3`.

## Install

```sh
cd fan-governor
sh install.sh
sudo systemctl enable --now fan-governor
journalctl -u fan-governor -f
```

| File                                          | Purpose                                                      |
|-----------------------------------------------|--------------------------------------------------------------|
| `/usr/local/bin/fan-governor.py`              | the daemon                                                   |
| `/etc/systemd/system/fan-governor.service`    | the unit                                                     |
| `/etc/fan-governor/config.example`            | reference config (re-installed every time)                   |
| `/etc/fan-governor/config.json`               | active config (created from example only if absent — your edits survive updates) |

## Configuration

Top-level keys (defaults in parentheses):

| Key                 | Default | Purpose                                                                            |
|---------------------|---------|------------------------------------------------------------------------------------|
| `poll_interval_s`   | `3`     | Seconds between ticks.                                                             |
| `min_pwm_step`      | `3`     | Skip writes if `|new − last| < this` AND less than `min_state_seconds` since last. |
| `min_state_seconds` | `5`     | Minimum time between PWM rewrites for an unchanged-ish group.                      |
| `emergency_temp`    | `88`    | °C. Source temp ≥ this → group forced to 100 %.                                    |
| `emergency_exit`    | `82`    | °C. Group leaves emergency only when source < this.                                |
| `gpu_index`         | `0`     | Which GPU to read temps from (`nvidia-smi -L`).                                    |
| `groups`            | —       | List of fan groups (see below). At least one required.                             |

### Group schema

```jsonc
{
  "name": "gpu",                 // free-form, must be unique within the config
  "source": "gpu_max",           // gpu_core | gpu_vram | gpu_max
  "targets": ["nvml:0:*"],       // see "Target syntax"
  "min_pwm": 30,                 // 0..100, optional (default 0)
  "curve": [[40,30],[60,50],[75,75],[83,100]]
}
```

### Target syntax

| Spec                | Meaning                                                              |
|---------------------|----------------------------------------------------------------------|
| `nvml:0:0`          | GPU 0, fan 0.                                                        |
| `nvml:0:*`          | GPU 0, all fans (count from `nvmlDeviceGetNumFans`).                 |
| `hwmon:it8688:pwm3` | sysfs PWM channel; chip name = exact value of `/sys/class/hwmon/*/name`. |

### Sources

| Source     | Backing call                                            |
|------------|---------------------------------------------------------|
| `gpu_core` | `nvmlDeviceGetTemperature(NVML_TEMPERATURE_GPU)`        |
| `gpu_vram` | `NVML_FI_DEV_MEMORY_TEMP` field — GDDR junction         |
| `gpu_max`  | `max(core, vram)` — recommended for RTX 30-series       |

If `gpu_vram` is unavailable on your driver/card, `gpu_max`
transparently falls back to `gpu_core` and the daemon logs a warning
once.

### Adding case fans (after `it87` is loaded)

Append a second group to the `groups` array in your config:

```jsonc
{
  "name": "case",
  "source": "gpu_max",
  "targets": ["hwmon:it8688:pwm3"],
  "min_pwm": 25,
  "curve": [[40, 25], [60, 40], [75, 60], [85, 80]]
}
```

…then `sudo systemctl restart fan-governor`.

## Common tuning recipes

### Quiet idle, hard ramp under load

```jsonc
"min_pwm": 25,
"curve": [[40, 25], [55, 30], [70, 60], [80, 100]]
```

### Always-aggressive cooling (training rigs)

```jsonc
"min_pwm": 50,
"curve": [[40, 50], [55, 65], [70, 85], [80, 100]]
```

### Avoid fan stop-start cycles

Raise `min_pwm` until your fans never stop spinning, and use a wide
`min_state_seconds` (e.g. `10`) so PWM writes are infrequent.

## Testing

### 1. Service is alive

```sh
systemctl status fan-governor
journalctl -u fan-governor -f
```

Expected first lines:

```
Config loaded from /etc/fan-governor/config.json
NVML init OK, managing GPU0 'NVIDIA GeForce RTX 3090' with 2 fans
group 'gpu' source=gpu_max targets=[nvml:0:0, nvml:0:1] curve=[40C->30%, ...] min_pwm=30
```

### 2. Curve responds to load

In one terminal:

```sh
journalctl -u fan-governor -f
```

In another, generate sustained GPU load. Within a few ticks:

```
group 'gpu' pwm 30% -> 55%
```

Cross-check externally:

```sh
nvidia-smi --query-gpu=fan.speed,temperature.gpu --format=csv
```

### 3. Emergency override

Temporarily lower `emergency_temp` to a value you'll cross under
load (e.g. `60`), restart, and verify the group is pinned at `100`
until temp drops below `emergency_exit`.

### 4. Restore on stop

```sh
sudo systemctl stop fan-governor
journalctl -u fan-governor | tail
nvidia-smi --query-gpu=fan.speed --format=csv          # firmware-driven again
cat /sys/class/hwmon/<chip>/pwmN_enable                # back to original mode (typ. 2)
```

### 5. Config validation rejects bad configs

```sh
# Break hysteresis on purpose:
sudo python3 -c '
import json, pathlib
p = pathlib.Path("/etc/fan-governor/config.json")
c = json.loads(p.read_text())
c["emergency_exit"] = 99
p.write_text(json.dumps(c, indent=2))
'
sudo systemctl restart fan-governor
systemctl status fan-governor      # exited 2; NOT auto-restarted
journalctl -u fan-governor | tail -3
# fix and restart
```

## Troubleshooting

- **`SetFanSpeed_v2 failed: Function Not Found`** — driver too old.
  `cat /proc/driver/nvidia/version`. Need ≥ 525, prefer ≥ 535.
- **`hwmon chip 'it8688' not found`** — `it87` not loaded. `lsmod | grep it87`,
  `dmesg | grep -i it87`, `sensors`. Re-do the DKMS step.
- **PWM writes succeed but the fan doesn't move** — wrong PWM
  channel for that header, or BIOS holds it. Check BIOS *Smart Fan*
  for that header; some boards force CPU_FAN to a fixed mode.
- **Fan stalls and won't restart** — raise `min_pwm` until it
  reliably starts from a stop.
- **Fan oscillation** — your curve is too steep around the current
  operating temperature, or `min_pwm_step` / `min_state_seconds` are
  too low.
- **Service won't start** — `journalctl -u fan-governor | tail`.
  Validation errors exit code 2 and the unit will *not* auto-restart
  (`RestartPreventExitStatus=2`).
- **Daemon crashed and left fans pinned** — restart the service; on
  init it claims and on shutdown it restores. To reset by hand:
  `nvidia-smi --query-gpu=fan.speed --format=csv`, and for hwmon
  channels `echo 2 | sudo tee /sys/class/hwmon/<chip>/pwmN_enable`.

## Diagnostic tools

[`tools/gpu-fan.sh`](tools/gpu-fan.sh) and [`tools/case-fan.sh`](tools/case-fan.sh)
are one-shot helpers for hardware bring-up, separate from the daemon.
They are not installed and are intended for manual troubleshooting:

| Script             | Backend          | Use case                                                                            |
|--------------------|------------------|-------------------------------------------------------------------------------------|
| `gpu-fan.sh`       | NVML             | Quickly set/reset GPU fans by percent, ramp 30→60→90→100% to identify noise levels. |
| `case-fan.sh`      | hwmon (`it87`)   | Map silkscreen-labeled headers to `pwmN`, set/restore individual channels, panic-reset the chip. |

Stop the daemon (`sudo systemctl stop fan-governor`) before using these;
otherwise the daemon will overwrite your manual changes on its next tick.
Each script prints its own usage when invoked without arguments.

## Scope and non-goals

- **Single GPU.** Multi-GPU = multiple service instances.
- **No CPU-temperature source.** Easy to add via `coretemp` hwmon if
  needed; right now the workstation's heat budget is GPU-dominated,
  so it's left out per the project's lean-by-default stance.
- **No process awareness** — pure thermal feedback.
- **Not a curve editor.** Edit the JSON, restart the service.
