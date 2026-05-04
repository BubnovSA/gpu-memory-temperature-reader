#!/usr/bin/env python3
"""Fan Governor — temperature-driven fan curve daemon.

Reads GPU temperatures via NVML and applies user-defined fan curves
to NVML-controlled GPU fans and hwmon-controlled motherboard PWM
channels.

Each "group" in the config defines:
  - a temperature source (gpu_core / gpu_vram / gpu_max),
  - a list of targets (nvml:<gpu>:<fan|*> or hwmon:<chip>:pwmN),
  - a curve of [temp_c, percent] points (linear interpolation,
    clamped at the ends),
  - an optional min_pwm floor (anti-stall).

Above emergency_temp the group is forced to 100% until temp drops
below emergency_exit. On graceful shutdown (SIGTERM/SIGINT) every
controlled fan is returned to firmware-default control.

Configuration is a JSON file (default /etc/fan-governor/config.json
or via --config / FAN_GOVERNOR_CONFIG).
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import signal
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

try:
    import pynvml
except ImportError:
    sys.stderr.write("ERROR: pynvml not installed. See README.\n")
    sys.exit(1)


DEFAULT_CONFIG_PATH = "/etc/fan-governor/config.json"
HEARTBEAT_INTERVAL_S = 300.0
HWMON_ROOT = Path("/sys/class/hwmon")

# NVML field id for GDDR memory temperature (stable across drivers).
NVML_FI_DEV_MEMORY_TEMP = 76


# ---------------- Config ----------------

@dataclass(frozen=True)
class Group:
    name: str
    source: str
    targets: Tuple[str, ...]
    curve: Tuple[Tuple[int, int], ...]
    min_pwm: int = 0


@dataclass(frozen=True)
class Config:
    poll_interval_s: int
    min_pwm_step: int
    min_state_seconds: int
    emergency_temp: int
    emergency_exit: int
    gpu_index: int
    groups: Tuple[Group, ...]

    @classmethod
    def load(cls, path: str) -> "Config":
        data = json.loads(Path(path).read_text())
        groups = []
        for g in data["groups"]:
            curve = tuple(sorted(
                ((int(t), int(p)) for t, p in g["curve"]),
                key=lambda x: x[0],
            ))
            groups.append(Group(
                name=g["name"],
                source=g["source"],
                targets=tuple(g["targets"]),
                curve=curve,
                min_pwm=int(g.get("min_pwm", 0)),
            ))
        return cls(
            poll_interval_s   = int(data.get("poll_interval_s", 3)),
            min_pwm_step      = int(data.get("min_pwm_step", 3)),
            min_state_seconds = int(data.get("min_state_seconds", 5)),
            emergency_temp    = int(data.get("emergency_temp", 88)),
            emergency_exit    = int(data.get("emergency_exit", 82)),
            gpu_index         = int(data.get("gpu_index", 0)),
            groups            = tuple(groups),
        )

    def validate(self) -> None:
        if self.poll_interval_s < 1:
            raise ValueError("poll_interval_s must be >= 1")
        if self.emergency_exit >= self.emergency_temp:
            raise ValueError("emergency_exit must be < emergency_temp (hysteresis)")
        if not self.groups:
            raise ValueError("at least one group required")
        seen = set()
        for g in self.groups:
            if g.name in seen:
                raise ValueError(f"duplicate group name: {g.name}")
            seen.add(g.name)
            if g.source not in ("gpu_core", "gpu_vram", "gpu_max"):
                raise ValueError(f"group {g.name}: unknown source {g.source!r}")
            if not g.targets:
                raise ValueError(f"group {g.name}: no targets")
            if len(g.curve) < 2:
                raise ValueError(f"group {g.name}: curve must have >= 2 points")
            for _, pct in g.curve:
                if not 0 <= pct <= 100:
                    raise ValueError(f"group {g.name}: pwm percent out of range")
            if not 0 <= g.min_pwm <= 100:
                raise ValueError(f"group {g.name}: min_pwm out of range")


# ---------------- Curve ----------------

def interpolate(curve: Tuple[Tuple[int, int], ...], temp: float) -> int:
    """Linear interpolation. curve must be sorted by temp; clamps at ends."""
    if temp <= curve[0][0]:
        return curve[0][1]
    if temp >= curve[-1][0]:
        return curve[-1][1]
    for (t0, p0), (t1, p1) in zip(curve, curve[1:]):
        if t0 <= temp <= t1:
            if t1 == t0:
                return p1
            return int(p0 + (p1 - p0) * (temp - t0) / (t1 - t0))
    return curve[-1][1]  # unreachable given the clamps above


# ---------------- Targets ----------------

class Target:
    name: str

    def apply(self, percent: int) -> None: ...
    def restore(self) -> None: ...


class NVMLFan(Target):
    def __init__(self, handle, gpu_index: int, fan_index: int):
        self.handle = handle
        self.gpu_index = gpu_index
        self.fan_index = fan_index
        self.name = f"nvml:{gpu_index}:{fan_index}"

    def apply(self, percent: int) -> None:
        try:
            pynvml.nvmlDeviceSetFanSpeed_v2(self.handle, self.fan_index, percent)
        except pynvml.NVMLError as e:
            logging.error("%s: SetFanSpeed_v2(%d) failed: %s", self.name, percent, e)

    def restore(self) -> None:
        try:
            pynvml.nvmlDeviceSetDefaultFanSpeed_v2(self.handle, self.fan_index)
            logging.info("%s: returned to firmware control", self.name)
        except pynvml.NVMLError as e:
            logging.error("%s: SetDefaultFanSpeed_v2 failed: %s", self.name, e)


class HwmonPwm(Target):
    def __init__(self, chip: str, pwm_name: str, pwm_path: Path, enable_path: Path):
        self.chip = chip
        self.pwm_name = pwm_name
        self.pwm_path = pwm_path
        self.enable_path = enable_path
        self.name = f"hwmon:{chip}:{pwm_name}"
        self._original_enable: Optional[str] = None

    def claim(self) -> None:
        """Snapshot the firmware mode, then switch to manual (pwmN_enable=1)."""
        try:
            self._original_enable = self.enable_path.read_text().strip()
        except OSError as e:
            logging.warning("%s: could not read %s: %s",
                            self.name, self.enable_path, e)
        try:
            self.enable_path.write_text("1")
        except OSError as e:
            logging.error("%s: cannot enable manual PWM: %s", self.name, e)
            raise

    def apply(self, percent: int) -> None:
        raw = max(0, min(255, round(percent * 255 / 100)))
        try:
            self.pwm_path.write_text(str(raw))
        except OSError as e:
            logging.error("%s: write %d (%d%%) failed: %s",
                          self.name, raw, percent, e)

    def restore(self) -> None:
        # Prefer the original mode we snapshotted; fall back to "2" (auto on it87/nct67xx).
        target = self._original_enable if self._original_enable is not None else "2"
        try:
            self.enable_path.write_text(target)
            logging.info("%s: returned to mode %s", self.name, target)
        except OSError as e:
            logging.error("%s: restore failed: %s", self.name, e)


# ---------------- Discovery ----------------

def discover_hwmon_chips() -> dict:
    """Return {chip_name: hwmon_path} from /sys/class/hwmon/*/name."""
    chips: dict = {}
    if not HWMON_ROOT.exists():
        return chips
    for entry in HWMON_ROOT.iterdir():
        try:
            name = (entry / "name").read_text().strip()
        except OSError:
            continue
        chips[name] = entry
    return chips


def resolve_targets(group: Group, gpu_handle, gpu_index: int) -> List[Target]:
    out: List[Target] = []
    chips = discover_hwmon_chips()
    fan_count = pynvml.nvmlDeviceGetNumFans(gpu_handle)

    for spec in group.targets:
        parts = spec.split(":")
        kind = parts[0]
        if kind == "nvml":
            if len(parts) != 3:
                raise ValueError(f"bad target {spec!r} (expect nvml:<gpu>:<fan|*>)")
            if int(parts[1]) != gpu_index:
                raise ValueError(
                    f"{spec}: gpu index does not match config gpu_index={gpu_index}")
            if parts[2] == "*":
                for i in range(fan_count):
                    out.append(NVMLFan(gpu_handle, gpu_index, i))
            else:
                i = int(parts[2])
                if not 0 <= i < fan_count:
                    raise ValueError(f"{spec}: fan index out of range (count={fan_count})")
                out.append(NVMLFan(gpu_handle, gpu_index, i))
        elif kind == "hwmon":
            if len(parts) != 3:
                raise ValueError(f"bad target {spec!r} (expect hwmon:<chip>:pwmN)")
            chip, pwm_name = parts[1], parts[2]
            if chip not in chips:
                raise ValueError(
                    f"{spec}: hwmon chip {chip!r} not found. "
                    f"Available: {sorted(chips)}")
            pwm_path = chips[chip] / pwm_name
            enable_path = chips[chip] / f"{pwm_name}_enable"
            if not pwm_path.exists() or not enable_path.exists():
                raise ValueError(f"{spec}: {pwm_path} or {enable_path} missing")
            out.append(HwmonPwm(chip, pwm_name, pwm_path, enable_path))
        else:
            raise ValueError(f"unknown target kind {kind!r} in {spec!r}")
    return out


# ---------------- Temperature source ----------------

class GpuTempSource:
    def __init__(self, handle):
        self.handle = handle
        self._vram_warned = False

    def core(self) -> int:
        return pynvml.nvmlDeviceGetTemperature(
            self.handle, pynvml.NVML_TEMPERATURE_GPU)

    def vram(self) -> Optional[int]:
        # pynvml >=12.x changed API: nvmlDeviceGetFieldValues(handle, [fieldId])
        # Old API was: nvmlDeviceGetFieldValues(handle, count, ctypes_array)
        # Try new API first; fall back to old on TypeError.
        try:
            result = pynvml.nvmlDeviceGetFieldValues(
                self.handle, [NVML_FI_DEV_MEMORY_TEMP])
            if not result or result[0].nvmlReturn != 0:
                if not self._vram_warned:
                    status = result[0].nvmlReturn if result else -1
                    logging.warning("VRAM temp field returned NVML status %d", status)
                    self._vram_warned = True
                return None
            val = result[0].value
            return int(val.siVal) if hasattr(val, 'siVal') else int(val)
        except TypeError:
            # Old pynvml ctypes API.
            try:
                FieldValue = pynvml.c_nvmlFieldValue_t
                arr = (FieldValue * 1)()
                arr[0].fieldId = NVML_FI_DEV_MEMORY_TEMP
                pynvml.nvmlDeviceGetFieldValues(self.handle, 1, arr)
                if arr[0].nvmlReturn != 0:
                    if not self._vram_warned:
                        logging.warning("VRAM temp field unsupported (NVML status %d)",
                                        arr[0].nvmlReturn)
                        self._vram_warned = True
                    return None
                return int(arr[0].value.siVal)
            except (pynvml.NVMLError, AttributeError, Exception) as e:
                if not self._vram_warned:
                    logging.warning("VRAM temp (old API) failed: %s", e)
                    self._vram_warned = True
                return None
        except pynvml.NVMLError as e:
            if not self._vram_warned:
                logging.warning("VRAM temp query failed: %s", e)
                self._vram_warned = True
            return None

    def read(self, source: str) -> Optional[float]:
        if source == "gpu_core":
            return float(self.core())
        if source == "gpu_vram":
            v = self.vram()
            return None if v is None else float(v)
        if source == "gpu_max":
            c = self.core()
            v = self.vram()
            return float(max(c, v)) if v is not None else float(c)
        return None


# ---------------- Group state ----------------

@dataclass
class GroupState:
    group: Group
    targets: List[Target]
    last_pwm: Optional[int] = None
    last_apply_t: float = 0.0
    in_emergency: bool = False


# ---------------- Governor ----------------

class FanGovernor:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.handle = None
        self.temp_source: Optional[GpuTempSource] = None
        self.states: List[GroupState] = []
        self._running = True
        self._restored = False
        self._last_heartbeat = 0.0

    def init(self) -> None:
        pynvml.nvmlInit()
        count = pynvml.nvmlDeviceGetCount()
        if self.cfg.gpu_index >= count:
            raise RuntimeError(
                f"GPU index {self.cfg.gpu_index} out of range (count={count})")
        self.handle = pynvml.nvmlDeviceGetHandleByIndex(self.cfg.gpu_index)
        name = pynvml.nvmlDeviceGetName(self.handle)
        gpu_name = name.decode() if isinstance(name, bytes) else name
        fan_count = pynvml.nvmlDeviceGetNumFans(self.handle)
        logging.info(
            "NVML init OK, managing GPU%d '%s' with %d fans",
            self.cfg.gpu_index, gpu_name, fan_count)
        self.temp_source = GpuTempSource(self.handle)

        for g in self.cfg.groups:
            targets = resolve_targets(g, self.handle, self.cfg.gpu_index)
            for t in targets:
                if isinstance(t, HwmonPwm):
                    t.claim()
            logging.info(
                "group %r source=%s targets=[%s] curve=[%s] min_pwm=%d",
                g.name, g.source,
                ", ".join(t.name for t in targets),
                ", ".join(f"{t}C->{p}%" for t, p in g.curve),
                g.min_pwm)
            self.states.append(GroupState(group=g, targets=targets))

    def _decide_pwm(self, st: GroupState, temp: float) -> int:
        if temp >= self.cfg.emergency_temp:
            st.in_emergency = True
        elif st.in_emergency and temp < self.cfg.emergency_exit:
            st.in_emergency = False
        if st.in_emergency:
            return 100
        return max(st.group.min_pwm, interpolate(st.group.curve, temp))

    def _apply_group(self, st: GroupState, pwm: int, now: float) -> None:
        # Rate-limit: skip if delta is small AND too soon since last write.
        # If pwm hasn't changed but min_state_seconds has passed, refresh
        # anyway — defends against external interference.
        if st.last_pwm is not None:
            delta = abs(pwm - st.last_pwm)
            since = now - st.last_apply_t
            if delta < self.cfg.min_pwm_step and since < self.cfg.min_state_seconds:
                return
        for t in st.targets:
            t.apply(pwm)
        if st.last_pwm != pwm:
            logging.info("group %r pwm %s -> %d%%",
                         st.group.name,
                         "?" if st.last_pwm is None else f"{st.last_pwm}%",
                         pwm)
        st.last_pwm = pwm
        st.last_apply_t = now

    def tick(self) -> None:
        now = time.monotonic()

        if now - self._last_heartbeat >= HEARTBEAT_INTERVAL_S:
            try:
                core = self.temp_source.core()
                vram = self.temp_source.vram()
            except pynvml.NVMLError as e:
                logging.error("NVML read failed: %s", e)
                return
            logging.info(
                "alive: core=%dC vram=%sC groups=[%s]",
                core,
                "?" if vram is None else str(vram),
                ", ".join(f"{s.group.name}={s.last_pwm}" for s in self.states))
            self._last_heartbeat = now

        for st in self.states:
            try:
                temp = self.temp_source.read(st.group.source)
            except pynvml.NVMLError as e:
                logging.error("group %r: temp read failed: %s", st.group.name, e)
                continue
            if temp is None:
                logging.warning("group %r: source %s unavailable, skipping",
                                st.group.name, st.group.source)
                continue
            pwm = self._decide_pwm(st, temp)
            self._apply_group(st, pwm, now)

    def install_signal_handlers(self) -> None:
        signal.signal(signal.SIGTERM, self._handle_signal)
        signal.signal(signal.SIGINT, self._handle_signal)

    def run(self) -> None:
        while self._running:
            try:
                self.tick()
            except Exception:
                logging.exception("Unhandled error in tick loop")
            slept = 0
            while self._running and slept < self.cfg.poll_interval_s:
                time.sleep(1)
                slept += 1

    def _handle_signal(self, signum, _frame) -> None:
        logging.info("Signal %d received, stopping", signum)
        self._running = False

    def restore(self) -> None:
        if self._restored:
            return
        logging.info("Restoring fan defaults")
        for st in self.states:
            for t in st.targets:
                try:
                    t.restore()
                except Exception:
                    logging.exception("restore failed for %s", t.name)
        self._restored = True

    def shutdown(self) -> None:
        try:
            self.restore()
        finally:
            try:
                pynvml.nvmlShutdown()
                logging.info("NVML shutdown")
            except pynvml.NVMLError as e:
                logging.warning("nvmlShutdown: %s", e)


# ---------------- Main ----------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Fan Governor")
    p.add_argument(
        "--config",
        default=os.environ.get("FAN_GOVERNOR_CONFIG", DEFAULT_CONFIG_PATH),
        help=f"path to JSON config (default: {DEFAULT_CONFIG_PATH})",
    )
    return p.parse_args()


def main() -> int:
    logging.basicConfig(
        stream=sys.stderr,
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    if os.geteuid() != 0:
        logging.error("fan-governor must run as root (NVML/hwmon writes require it)")
        return 2

    args = parse_args()
    try:
        cfg = Config.load(args.config)
        cfg.validate()
    except (FileNotFoundError, json.JSONDecodeError, ValueError, KeyError) as e:
        logging.error("Config error (%s): %s", args.config, e)
        return 2

    logging.info("Config loaded from %s", args.config)
    gov = FanGovernor(cfg)
    gov.install_signal_handlers()
    try:
        gov.init()
        gov.run()
        return 0
    except Exception:
        logging.exception("Fatal error")
        return 1
    finally:
        gov.shutdown()


if __name__ == "__main__":
    sys.exit(main())
