# NVIDIA GPU Toolkit for Linux

Three independent utilities for monitoring and managing NVIDIA GPUs on a
Linux workstation or single-node AI machine:

| Utility                                    | What it does                                                                                                                                | Docs                                              |
|--------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------|
| [`gddr6`](gddr6-monitor/README.md)         | Real-time VRAM temperature readout + GPU/memory clocks, GPU/memory utilization, CSV logging.                                                | [gddr6-monitor/README.md](gddr6-monitor/README.md) |
| [`gpu-governor`](gpu-governor/README.md)   | `systemd`-managed daemon that caps GPU power and locks core-clock ceiling under load, backs off on overheat, restores defaults on shutdown. | [gpu-governor/README.md](gpu-governor/README.md)  |
| [`fan-governor`](fan-governor/README.md)   | `systemd`-managed daemon driving GPU fans (NVML) and motherboard PWM (hwmon) from per-group temperature curves; restores firmware control on shutdown. | [fan-governor/README.md](fan-governor/README.md)  |

They are complementary and independent. `gddr6` observes; `gpu-governor`
caps power and clocks; `fan-governor` drives fans. All three can run
together without conflict.

## Supported GPUs

### `gddr6` (VRAM readout)

Temperature is read from a reverse-engineered MMIO offset, which is
architecture-specific. Currently validated on:

- **Ada Lovelace (RTX 40-series):** 4090, 4080 Super, 4080, 4070 Ti Super,
  4070 Ti, 4070 Super, 4070
- **Ampere GA102:** 3090 Ti, 3090, 3080 Ti, 3080, 3080 LHR, A4500, A5000, A10
- **Ampere GA104:** 3070, 3070 LHR
- **Ampere GA106:** A2000
- **Datacenter Ada:** L4, L40S, A6000

GPU/memory clocks and utilization are read via NVML and work on any
NVML-supported card.

### `gpu-governor` (power/clock management)

Works with any NVIDIA GPU that NVML supports (Kepler+). Clock locking
(`nvmlDeviceSetGpuLockedClocks`) requires Turing (RTX 20xx) or newer; older
cards fall back to power-cap-only mode automatically.

### `fan-governor` (fan control)

Requires NVIDIA driver ≥ 525 (`nvmlDeviceSetFanSpeed_v2`); verified on
580.x. Motherboard PWM control is optional and depends on a working
hwmon driver for your super-I/O chip (e.g. out-of-tree `it87` for
IT8688E on Gigabyte boards).

## Shared system prerequisites

All three utilities need privileged access to the GPU. The following apply
repo-wide:

### Secure Boot off

```sh
sudo mokutil --disable-validation
sudo mokutil --sb   # should print: SecureBoot disabled
```

### `iomem=relaxed` kernel parameter (for `gddr6` only)

Some distros require it to allow MMIO mapping from `/dev/mem`. On Debian /
Ubuntu:

```sh
sudo vim /etc/default/grub
# GRUB_CMDLINE_LINUX_DEFAULT="quiet splash iomem=relaxed"
sudo update-grub
sudo reboot
```

Other distros may boot with sufficient defaults already — try `gddr6` first
and only apply this if the mapping fails.

## Per-utility prerequisites, install, usage, testing

See the sub-README for each utility:

- `gddr6`: build deps, CLI flags, CSV log format — [gddr6-monitor/README.md](gddr6-monitor/README.md)
- `gpu-governor`: Python deps, systemd unit, config reference, state machine — [gpu-governor/README.md](gpu-governor/README.md)
- `fan-governor`: fan curves, hwmon/NVML targets, config reference — [fan-governor/README.md](fan-governor/README.md)

## Repository layout

```
lib/                        # gddr6 static library (libpci + /dev/mem mmap)
gddr6-monitor/              # gddr6 CLI (C)
  src/                        app.c + logger.c + nvml_probe.c
  install.sh                  installer (cmake → build → install)
  README.md                   gddr6 usage + testing
gpu-governor/               # gpu-governor daemon (Python)
  gpu-governor.py             daemon entry point
  gpu-governor.service        systemd unit
  config.example              reference config
  install.sh                  installer
  README.md                   governor usage + testing
fan-governor/               # fan-governor daemon (Python)
  fan-governor.py             daemon entry point
  fan-governor.service        systemd unit
  config.example              reference config (JSON)
  install.sh                  installer
  README.md                   governor usage + testing
CMakeLists.txt              # top-level build entry (lib + gddr6 monitor)
```

## Testing strategy

There is no automated CI — all three tools are tightly coupled to real
hardware and privileged kernel/driver APIs. Testing is manual, performed on
the target Linux machine, and scoped per utility.

Each sub-README has a `## Testing` section with numbered, copy-pasteable
commands and expected output. The recommended workflow after changes:

1. Build / reinstall the affected utility (see its README's *Update* section).
2. Run through the *Testing* section top to bottom.
3. Only commit after every relevant check passes.

## Credits

VRAM temperature readout technique based on the original work at
[olealgoritme/gddr6](https://github.com/olealgoritme/gddr6).

![](https://github.com/olealgoritme/gddr6/blob/master/gddr6_use.gif)
