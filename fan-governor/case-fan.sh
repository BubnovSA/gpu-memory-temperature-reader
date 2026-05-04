#!/bin/bash
# case-fan.sh — quick hwmon (it87) utilities for IT8688E.
# Verified target: Gigabyte H470 AORUS PRO AX.
#
# Unlike NVML, hwmon manual mode is PERSISTENT until reboot or explicit `auto`.
# The `find` subcommand always restores touched channels, even on Ctrl+C.
#
# One-time install (header of this file is the runbook):
#   sudo apt install -y lm-sensors dkms git build-essential linux-headers-$(uname -r)
#   git clone https://github.com/frankcrawford/it87.git ~/it87
#   cd ~/it87 && sudo make dkms
#   echo 'options it87 force_id=0x8688 ignore_resource_conflict=1' | sudo tee /etc/modprobe.d/it87.conf
#   echo it87 | sudo tee /etc/modules-load.d/it87.conf
#   sudo modprobe it87 force_id=0x8688 ignore_resource_conflict=1
#   sensors | grep it8688          # verify chip is visible
#
# Usage:
#   sudo ./case-fan.sh                       → status of all pwm/fan channels
#   sudo ./case-fan.sh find                  → ramp each pwm to 90% one-by-one (4s)
#   sudo ./case-fan.sh set <N> <PCT>         → set pwmN to PCT%
#   sudo ./case-fan.sh auto <N>              → return pwmN to auto
#   sudo ./case-fan.sh auto-all              → return all pwm channels to auto
#   sudo ./case-fan.sh watch                 → live status, refresh every 1s
#
set -eu

[ "$(id -u)" -eq 0 ] || { echo "ERROR: run with sudo" >&2; exit 1; }

CHIP_FILE=$(grep -l '^it8688$' /sys/class/hwmon/*/name 2>/dev/null | head -1 || true)
if [ -z "$CHIP_FILE" ]; then
    echo "ERROR: it8688 chip not found in /sys/class/hwmon/" >&2
    echo "       Is it87 module loaded?  lsmod | grep it87  ;  sensors" >&2
    exit 1
fi
CHIP=$(dirname "$CHIP_FILE")

pwm_list() {
    for N in 1 2 3 4 5; do
        [ -e "$CHIP/pwm$N" ] && echo "$N"
    done
}

show_status() {
    echo "chip: $CHIP"
    for N in $(pwm_list); do
        PWM=$(cat "$CHIP/pwm$N")
        EN=$(cat "$CHIP/pwm${N}_enable")
        RPM=$(cat "$CHIP/fan${N}_input" 2>/dev/null || echo -)
        printf "pwm%d: raw=%-3d (%-3d%%)  mode=%s  rpm=%s\n" \
            "$N" "$PWM" "$((PWM*100/255))" "$EN" "$RPM"
    done
}

case "${1:-status}" in
status)
    show_status
    ;;
set)
    N=${2:-}; PCT=${3:-}
    case "$N" in ''|*[!0-9]*) echo "Usage: $0 set <pwmN> <0..100>" >&2; exit 2 ;; esac
    case "$PCT" in ''|*[!0-9]*) echo "Usage: $0 set <pwmN> <0..100>" >&2; exit 2 ;; esac
    [ "$PCT" -le 100 ] || { echo "PCT must be 0..100" >&2; exit 2; }
    [ -e "$CHIP/pwm$N" ] || { echo "pwm$N does not exist" >&2; exit 2; }
    RAW=$((PCT * 255 / 100))
    echo 1     > "$CHIP/pwm${N}_enable"
    echo "$RAW" > "$CHIP/pwm$N"
    echo "pwm$N -> $PCT% (raw=$RAW)"
    show_status
    ;;
auto)
    N=${2:-}
    [ -n "$N" ] || { echo "Usage: $0 auto <pwmN>" >&2; exit 2; }
    [ -e "$CHIP/pwm${N}_enable" ] || { echo "pwm$N does not exist" >&2; exit 2; }
    echo 2 > "$CHIP/pwm${N}_enable"
    echo "pwm$N -> auto"
    show_status
    ;;
auto-all)
    for N in $(pwm_list); do echo 2 > "$CHIP/pwm${N}_enable"; done
    echo "all pwm channels -> auto"
    show_status
    ;;
find)
    declare -A ORIG
    cleanup_find() {
        trap - INT TERM EXIT
        echo
        for N in "${!ORIG[@]}"; do
            echo "${ORIG[$N]}" > "$CHIP/pwm${N}_enable" 2>/dev/null || true
        done
        echo "all touched pwm channels restored"
        exit 0
    }
    trap cleanup_find INT TERM EXIT
    for N in $(pwm_list); do
        ORIG[$N]=$(cat "$CHIP/pwm${N}_enable")
        echo "=== pwm$N at 90% — listen 4s ==="
        echo 1   > "$CHIP/pwm${N}_enable"
        echo 229 > "$CHIP/pwm$N"
        sleep 4
        RPM=$(cat "$CHIP/fan${N}_input" 2>/dev/null || echo -)
        echo "    rpm=$RPM"
        echo "${ORIG[$N]}" > "$CHIP/pwm${N}_enable"
        unset 'ORIG[$N]'
        sleep 1
    done
    ;;
watch)
    exec watch -n 1 "$0 status"
    ;;
*)
    sed -n '2,/^set -eu/p' "$0" | sed 's/^# \{0,1\}//; /^set -eu/d'
    exit 2
    ;;
esac
