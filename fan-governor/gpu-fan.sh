#!/bin/bash
# gpu-fan.sh — quick NVML fan utilities for RTX 30xx.
# Verified on driver 580.159.03 + RTX 3090 (2 fans).
#
# All commands below were tested and confirmed working on this machine.
# NVML manual mode is non-persistent: driver reload / reboot returns fans
# to firmware automatically, so there is no "permanently stuck" failure mode.
#
# Usage:
#   sudo ./gpu-fan.sh                  → status (temps, fan.speed, power)
#   sudo ./gpu-fan.sh set <PCT>        → set both fans to PCT% (0..100)
#   sudo ./gpu-fan.sh auto             → return to firmware control
#   sudo ./gpu-fan.sh ramp             → 30→60→90→100→auto, 4s each (sound test)
#   sudo ./gpu-fan.sh watch            → live status, refreshes every 1s
#
# To install the full curve-driven daemon (recommended after testing):
#   sudo apt install -y python3-pynvml
#   cd fan-governor && sh install.sh
#   sudo systemctl enable --now fan-governor
#   journalctl -u fan-governor -f
#
# To stop the daemon and return GPU fans to firmware control:
#   sudo systemctl stop fan-governor
#
set -eu

[ "$(id -u)" -eq 0 ] || { echo "ERROR: run with sudo" >&2; exit 1; }

set_fans() {
    local pct=$1
    python3 - "$pct" <<'PY'
import sys, pynvml as p
pct = int(sys.argv[1])
p.nvmlInit()
h = p.nvmlDeviceGetHandleByIndex(0)
n = p.nvmlDeviceGetNumFans(h)
for i in range(n):
    p.nvmlDeviceSetFanSpeed_v2(h, i, pct)
print(f"set {n} fans to {pct}%")
PY
}

restore_fans() {
    python3 - <<'PY'
import pynvml as p
p.nvmlInit()
h = p.nvmlDeviceGetHandleByIndex(0)
n = p.nvmlDeviceGetNumFans(h)
for i in range(n):
    p.nvmlDeviceSetDefaultFanSpeed_v2(h, i)
print(f"restored {n} fans to firmware control")
PY
}

show_status() {
    nvidia-smi --query-gpu=temperature.gpu,temperature.memory,fan.speed,power.draw \
               --format=csv
}

case "${1:-status}" in
status)
    show_status
    ;;
set)
    PCT=${2:-}
    case "$PCT" in
        ''|*[!0-9]*) echo "Usage: $0 set <0..100>" >&2; exit 2 ;;
    esac
    [ "$PCT" -le 100 ] || { echo "PCT must be 0..100" >&2; exit 2; }
    set_fans "$PCT"
    show_status
    ;;
auto)
    restore_fans
    show_status
    ;;
ramp)
    python3 - <<'PY'
import pynvml as p, time
p.nvmlInit()
h = p.nvmlDeviceGetHandleByIndex(0)
n = p.nvmlDeviceGetNumFans(h)
try:
    for pct in (30, 60, 90, 100):
        for i in range(n):
            p.nvmlDeviceSetFanSpeed_v2(h, i, pct)
        print(f"GPU fans -> {pct}%  (listening 4s)")
        time.sleep(4)
finally:
    for i in range(n):
        p.nvmlDeviceSetDefaultFanSpeed_v2(h, i)
    print("restored to firmware control")
PY
    ;;
watch)
    exec watch -n 1 'nvidia-smi --query-gpu=temperature.gpu,temperature.memory,fan.speed,power.draw --format=csv'
    ;;
*)
    sed -n '2,/^set -eu/p' "$0" | sed 's/^# \{0,1\}//; /^set -eu/d'
    exit 2
    ;;
esac
