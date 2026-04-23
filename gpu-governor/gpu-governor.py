#!/usr/bin/env python3
"""GPU Governor — NVML power/clock manager with thermal protection.

State machine:
    IDLE    — no active management (clocks and power limit at default)
    COMPUTE — power capped + GPU clock locked (efficiency point)
    THERMAL — reduced power cap while GPU cools down

Transitions use hysteresis on temperature and a rolling-average utilization
window to avoid thrashing.  On graceful shutdown (SIGTERM / SIGINT) factory
defaults are restored so the GPU is left in a sane state.

Configuration is read from environment variables (see README).
"""
from __future__ import annotations

import collections
import logging
import os
import signal
import sys
import time
from dataclasses import dataclass
from typing import Optional

try:
    import pynvml
except ImportError:
    sys.stderr.write("ERROR: pynvml not installed. Run: pip3 install nvidia-ml-py\n")
    sys.exit(1)


# ---------------- Config ----------------

def _env_int(name: str, default: int) -> int:
    v = os.environ.get(name)
    if not v:
        return default
    try:
        return int(v)
    except ValueError:
        logging.warning("Invalid int for %s=%r, using default %d", name, v, default)
        return default


@dataclass(frozen=True)
class Config:
    gpu_index: int
    poll_interval_s: int
    util_high: int
    util_low: int
    util_window: int
    state_min_seconds: int
    temp_enter: int
    temp_exit: int
    pl_compute_w: int
    pl_thermal_w: int
    clock_min_mhz: int
    clock_max_mhz: int

    @classmethod
    def from_env(cls) -> "Config":
        return cls(
            gpu_index         = _env_int("GPU_GOVERNOR_INDEX",             0),
            poll_interval_s   = _env_int("GPU_GOVERNOR_POLL_INTERVAL",     5),
            util_high         = _env_int("GPU_GOVERNOR_UTIL_HIGH",        25),
            util_low          = _env_int("GPU_GOVERNOR_UTIL_LOW",         10),
            util_window       = _env_int("GPU_GOVERNOR_UTIL_WINDOW",       3),
            state_min_seconds = _env_int("GPU_GOVERNOR_STATE_MIN_SECONDS",15),
            temp_enter        = _env_int("GPU_GOVERNOR_TEMP_ENTER",       83),
            temp_exit         = _env_int("GPU_GOVERNOR_TEMP_EXIT",        78),
            pl_compute_w      = _env_int("GPU_GOVERNOR_PL_COMPUTE",      290),
            pl_thermal_w      = _env_int("GPU_GOVERNOR_PL_THERMAL",      160),
            clock_min_mhz     = _env_int("GPU_GOVERNOR_CLOCK_MIN",       210),
            clock_max_mhz     = _env_int("GPU_GOVERNOR_CLOCK_MAX",      1800),
        )

    def validate(self) -> None:
        if self.util_low >= self.util_high:
            raise ValueError("UTIL_LOW must be < UTIL_HIGH")
        if self.temp_exit >= self.temp_enter:
            raise ValueError("TEMP_EXIT must be < TEMP_ENTER (required for hysteresis)")
        if self.clock_min_mhz >= self.clock_max_mhz:
            raise ValueError("CLOCK_MIN must be < CLOCK_MAX")
        if self.poll_interval_s < 1:
            raise ValueError("POLL_INTERVAL must be >= 1")
        if self.util_window < 1:
            raise ValueError("UTIL_WINDOW must be >= 1")


# ---------------- States ----------------

IDLE = "IDLE"
COMPUTE = "COMPUTE"
THERMAL = "THERMAL"


# ---------------- Governor ----------------

class Governor:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.handle = None
        self.gpu_name = "unknown"
        self.default_pl_mw: Optional[int] = None
        # use_locked_clocks: None=not probed, True=supported, False=unsupported
        self.use_locked_clocks: Optional[bool] = None
        self.clock_locked = False
        self.state = IDLE
        self.state_since = time.monotonic()
        self.util_window: "collections.deque[int]" = collections.deque(
            maxlen=cfg.util_window)
        self._running = True
        self._restored = False

    # ---- NVML init ----

    def init(self) -> None:
        pynvml.nvmlInit()
        count = pynvml.nvmlDeviceGetCount()
        if self.cfg.gpu_index >= count:
            raise RuntimeError(
                f"GPU index {self.cfg.gpu_index} out of range (count={count})")

        self.handle = pynvml.nvmlDeviceGetHandleByIndex(self.cfg.gpu_index)
        name = pynvml.nvmlDeviceGetName(self.handle)
        self.gpu_name = name.decode() if isinstance(name, bytes) else name
        logging.info("NVML init OK, managing GPU%d '%s'",
                     self.cfg.gpu_index, self.gpu_name)

        try:
            self.default_pl_mw = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(self.handle)
            logging.info("Default power limit: %d W", self.default_pl_mw // 1000)
        except pynvml.NVMLError as e:
            logging.warning("Could not read default power limit: %s", e)

        try:
            pynvml.nvmlDeviceSetPersistenceMode(self.handle, 1)
            logging.info("Persistence mode enabled")
        except pynvml.NVMLError as e:
            logging.warning("Persistence mode not set: %s (run as root?)", e)

    # ---- Action helpers ----

    def _set_power_limit(self, watts: int) -> None:
        try:
            pynvml.nvmlDeviceSetPowerManagementLimit(self.handle, watts * 1000)
            logging.info("Power limit -> %d W", watts)
        except pynvml.NVMLError as e:
            logging.error("SetPowerManagementLimit(%dW) failed: %s", watts, e)

    def _restore_power_limit(self) -> None:
        if self.default_pl_mw is None:
            return
        try:
            pynvml.nvmlDeviceSetPowerManagementLimit(self.handle, self.default_pl_mw)
            logging.info("Power limit -> default (%d W)", self.default_pl_mw // 1000)
        except pynvml.NVMLError as e:
            logging.error("Restore power limit failed: %s", e)

    def _lock_clock(self, mhz_min: int, mhz_max: int) -> None:
        if self.use_locked_clocks is False:
            return
        try:
            pynvml.nvmlDeviceSetGpuLockedClocks(self.handle, mhz_min, mhz_max)
            self.use_locked_clocks = True
            self.clock_locked = True
            logging.info("GPU clock locked to %d–%d MHz", mhz_min, mhz_max)
        except pynvml.NVMLError as e:
            if self.use_locked_clocks is None:
                self.use_locked_clocks = False
                logging.error(
                    "nvmlDeviceSetGpuLockedClocks not supported (%s); "
                    "continuing with power-cap only", e)
            else:
                logging.error("SetGpuLockedClocks(%d,%d) failed: %s",
                              mhz_min, mhz_max, e)

    def _unlock_clock(self) -> None:
        if not self.clock_locked or self.use_locked_clocks is not True:
            return
        try:
            pynvml.nvmlDeviceResetGpuLockedClocks(self.handle)
            logging.info("GPU clock unlocked")
        except pynvml.NVMLError as e:
            logging.error("ResetGpuLockedClocks failed: %s", e)
        finally:
            self.clock_locked = False

    # ---- State transitions ----

    def _transition(self, new_state: str) -> None:
        if new_state == self.state:
            return
        logging.info("STATE %s -> %s", self.state, new_state)
        self.state = new_state
        self.state_since = time.monotonic()

    def _enter_compute(self) -> None:
        self._set_power_limit(self.cfg.pl_compute_w)
        self._lock_clock(self.cfg.clock_min_mhz, self.cfg.clock_max_mhz)
        self._transition(COMPUTE)

    def _enter_idle(self) -> None:
        self._unlock_clock()
        self._restore_power_limit()
        self._transition(IDLE)

    def _enter_thermal(self) -> None:
        self._set_power_limit(self.cfg.pl_thermal_w)
        self._unlock_clock()
        self._transition(THERMAL)

    # ---- Main tick ----

    def _time_in_state(self) -> float:
        return time.monotonic() - self.state_since

    def _avg_util(self) -> Optional[float]:
        if not self.util_window:
            return None
        return sum(self.util_window) / len(self.util_window)

    def tick(self) -> None:
        try:
            util = pynvml.nvmlDeviceGetUtilizationRates(self.handle).gpu
            temp = pynvml.nvmlDeviceGetTemperature(
                self.handle, pynvml.NVML_TEMPERATURE_GPU)
        except pynvml.NVMLError as e:
            logging.error("NVML read failed: %s", e)
            return

        self.util_window.append(util)
        avg = self._avg_util() or 0.0

        logging.info(
            "util=%3d%% avg=%5.1f%% temp=%3d°C state=%-7s age=%4.0fs",
            util, avg, temp, self.state, self._time_in_state())

        # --- Priority 1: thermal entry (immediate, bypasses min-time) ---
        if temp >= self.cfg.temp_enter:
            if self.state != THERMAL:
                self._enter_thermal()
            return

        # --- Priority 2: thermal recovery (hysteresis + min time) ---
        if self.state == THERMAL:
            cooled = temp < self.cfg.temp_exit
            settled = self._time_in_state() >= self.cfg.state_min_seconds
            if cooled and settled:
                # Exit to IDLE; utilization-based transitions will pick up COMPUTE
                self._enter_idle()
            return

        # --- Priority 3: utilization-based (debounced) ---
        # Wait for full rolling window before making decisions.
        if len(self.util_window) < self.cfg.util_window:
            return
        # Respect min-time-in-state to avoid flapping.
        if self._time_in_state() < self.cfg.state_min_seconds:
            return

        if avg >= self.cfg.util_high and self.state != COMPUTE:
            self._enter_compute()
        elif avg <= self.cfg.util_low and self.state != IDLE:
            self._enter_idle()

    # ---- Lifecycle ----

    def run(self) -> None:
        signal.signal(signal.SIGTERM, self._handle_signal)
        signal.signal(signal.SIGINT, self._handle_signal)
        while self._running:
            try:
                self.tick()
            except Exception:
                logging.exception("Unhandled error in tick loop")
            # Sleep in small chunks so SIGTERM is responsive.
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
        logging.info("Restoring GPU defaults")
        self._unlock_clock()
        self._restore_power_limit()
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


def main() -> int:
    logging.basicConfig(
        stream=sys.stderr,
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    try:
        cfg = Config.from_env()
        cfg.validate()
    except ValueError as e:
        logging.error("Config error: %s", e)
        return 2

    logging.info("Config: %s", cfg)
    gov = Governor(cfg)
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
