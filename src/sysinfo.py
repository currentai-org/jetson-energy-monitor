"""
Background system-resource monitor using jetson-stats (jtop).

jtop's client maintains a persistent connection to the jtop system service
and blocks briefly on each attribute access while it waits for/parses the
next stats message, so we run it in its own background thread (like the
INA3221 Sampler) and hand the UI a simple immutable snapshot dataclass to
poll at whatever rate it likes. This keeps a slow/stalled jtop connection
from ever blocking the 1kHz current-sampling thread or the UI event loop.

Requires the jtop *service* (jetson-stats) already running system-wide, and
a client library version that matches it -- this project pins
`jetson-stats==4.3.2` in pyproject.toml to match the version bundled with
L4T 36.5.0 on pocket-infer-6a8f (`sudo jtop --install-service` would only
be needed if a version mismatch error appears after a package upgrade).
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field

try:
    from jtop import jtop, JtopException
except ImportError:  # pragma: no cover - allows the rest of the app to run
    jtop = None
    JtopException = Exception


@dataclass
class SysSnapshot:
    t: float = 0.0
    ok: bool = False
    error: str | None = None
    cpu_percent: float = 0.0  # aggregate, 0-100 (100 - idle)
    cpu_per_core: list[float] = field(default_factory=list)
    gpu_percent: float = 0.0
    ram_used_mb: float = 0.0
    ram_total_mb: float = 0.0
    ram_percent: float = 0.0
    swap_used_mb: float = 0.0
    swap_total_mb: float = 0.0
    swap_percent: float = 0.0
    fan_rpm: float | None = None
    fan_percent: float | None = None
    temp_c: float | None = None  # thermal-junction / hottest reported zone
    temp_zones: dict = field(default_factory=dict)


class SysMonitor:
    """Background thread polling jtop; exposes the latest SysSnapshot.

    Designed to degrade gracefully: if jtop isn't importable or the service
    isn't reachable, `latest` stays a default SysSnapshot(ok=False, error=...)
    and the UI can show "unavailable" rather than crashing.
    """

    def __init__(self, poll_hz: float = 2.0):
        self.poll_period_s = 1.0 / poll_hz
        self.latest = SysSnapshot()
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run, name="sysmonitor", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 2.0) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
            self._thread = None

    def _run(self) -> None:
        if jtop is None:
            self.latest = SysSnapshot(ok=False, error="jtop package not installed")
            return
        try:
            with jtop() as jetson:
                while not self._stop_event.is_set() and jetson.ok():
                    self.latest = self._snapshot_from(jetson)
                    self._stop_event.wait(self.poll_period_s)
        except JtopException as e:
            self.latest = SysSnapshot(ok=False, error=f"jtop error: {e}")
        except Exception as e:  # pragma: no cover - defensive
            self.latest = SysSnapshot(ok=False, error=f"unexpected: {e!r}")

    @staticmethod
    def _snapshot_from(jetson) -> SysSnapshot:
        cpu = jetson.cpu
        cpu_total = cpu.get("total", {})
        cpu_percent = 100.0 - cpu_total.get("idle", 100.0)
        per_core = [
            100.0 - c.get("idle", 100.0)
            for c in cpu.get("cpu", [])
            if c.get("online", False)
        ]

        gpu = jetson.gpu
        gpu_percent = 0.0
        try:
            # jtop's gpu dict is keyed by gpu name (usually "gpu" on Jetson)
            first_gpu = next(iter(gpu.values()))
            gpu_percent = float(first_gpu.get("status", {}).get("load", 0.0))
        except (StopIteration, AttributeError, TypeError):
            pass

        mem = jetson.memory
        ram = mem.get("RAM", {})
        ram_total_mb = ram.get("tot", 0) / 1024.0
        ram_used_mb = ram.get("used", 0) / 1024.0
        ram_percent = (ram_used_mb / ram_total_mb * 100.0) if ram_total_mb else 0.0

        swap = mem.get("SWAP", {})
        swap_total_mb = swap.get("tot", 0) / 1024.0
        swap_used_mb = swap.get("used", 0) / 1024.0
        swap_percent = (swap_used_mb / swap_total_mb * 100.0) if swap_total_mb else 0.0

        fan_rpm = None
        fan_percent = None
        try:
            fan = jetson.fan
            first_fan = next(iter(fan.values()))
            rpm_list = first_fan.get("rpm")
            speed_list = first_fan.get("speed")
            if rpm_list:
                fan_rpm = float(rpm_list[0])
            if speed_list:
                fan_percent = float(speed_list[0])
        except (StopIteration, AttributeError, TypeError, KeyError):
            pass

        temps = jetson.temperature
        temp_zones = {
            name: z.get("temp")
            for name, z in temps.items()
            if isinstance(z, dict) and z.get("online") and z.get("temp", -256) > -100
        }
        # "tj" (thermal junction) is the effective die temp used for throttling
        # on Jetson SoCs when present; fall back to the hottest online zone.
        if "tj" in temp_zones:
            temp_c = temp_zones["tj"]
        elif temp_zones:
            temp_c = max(temp_zones.values())
        else:
            temp_c = None

        return SysSnapshot(
            t=time.time(),
            ok=True,
            error=None,
            cpu_percent=cpu_percent,
            cpu_per_core=per_core,
            gpu_percent=gpu_percent,
            ram_used_mb=ram_used_mb,
            ram_total_mb=ram_total_mb,
            ram_percent=ram_percent,
            swap_used_mb=swap_used_mb,
            swap_total_mb=swap_total_mb,
            swap_percent=swap_percent,
            fan_rpm=fan_rpm,
            fan_percent=fan_percent,
            temp_c=temp_c,
            temp_zones=temp_zones,
        )
