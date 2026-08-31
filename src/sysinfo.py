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

CPU reading glitch workaround: the jtop *system service* (root-owned,
shared across all users of the device -- not something this project can
or should patch) periodically calls `CPUService.reset_estimation()` on
its internal `/proc/stat` delta tracker whenever its control queue goes
briefly idle (see `jtop/service.py`'s `queue.Empty` handler and
`jtop/core/cpu.py`'s `get_utilization()`). The very next CPU reading
after a reset computes its delta against an all-zero baseline instead of
the previous real sample, which yields the *cumulative average
utilization since boot* instead of an instantaneous reading. Confirmed
via direct raw jtop polling that this glitch value appears on **every
other** update (a strict 50% duty cycle square wave, e.g. always exactly
"3.78%" alternating with genuine live readings on pocket-infer-6a8f) --
not a rare one-off, so a median-of-N filter cannot reject it (it would
dominate any small window). GPU load and temperature come from
different, non-delta-based jtop code paths and are unaffected.

Fix: bypass jtop's shared CPU estimator entirely. `_ProcStatCpuReader`
below reads `/proc/stat` directly and keeps its own **private** delta
state (one instance per `SysMonitor`), so no other client sharing the
jtop service can ever reset it out from under us. This is the same
`/proc/stat` parsing jtop itself does internally (see `jtop/core/cpu.py`
if comparing), just with delta-tracking state that isn't shared process-
or service-wide. All non-CPU fields (GPU/RAM/swap/fan/temp) still come
from jtop as before.
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


class _ProcStatCpuReader:
    """Computes instantaneous CPU utilization directly from /proc/stat,
    maintaining its own private previous-sample state so it can never be
    disrupted by another process/thread's estimator being reset (see
    module docstring -- this is what jtop's shared service-side estimator
    is vulnerable to).

    /proc/stat format per CPU line (aggregate line has no number suffix):
        cpu<N> user nice system idle iowait irq softirq [steal guest ...]
    All fields are cumulative jiffy counts since boot; utilization is the
    delta over the polling interval, not the raw counters themselves.
    """

    def __init__(self, path: str = "/proc/stat"):
        self.path = path
        self._last_total: dict[int | None, list[float]] = {}

    def read(self) -> tuple[float, list[float]]:
        """Returns (aggregate_cpu_percent, per_core_cpu_percent_list)."""
        aggregate = 0.0
        per_core: list[float] = []
        with open(self.path) as f:
            for line in f:
                if not line.startswith("cpu"):
                    break  # all cpu* lines are contiguous at the top
                parts = line.split()
                label = parts[0]
                fields = [float(x) for x in parts[1:8]]  # user..softirq
                key: int | None = None if label == "cpu" else int(label[3:])
                pct = self._delta_percent(key, fields)
                if key is None:
                    aggregate = pct
                else:
                    per_core.append(pct)
        return aggregate, per_core

    def _delta_percent(self, key: int | None, fields: list[float]) -> float:
        prev = self._last_total.get(key)
        self._last_total[key] = fields
        if prev is None:
            return 0.0  # no baseline yet on the very first read
        delta = [now - old for now, old in zip(fields, prev)]
        total = sum(delta)
        if total <= 0:
            return 0.0
        idle = delta[3]  # index 3 = 'idle' in user/nice/system/idle/...
        return 100.0 * (1.0 - idle / total)


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
        self._cpu_reader = _ProcStatCpuReader()

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

    def _snapshot_from(self, jetson) -> SysSnapshot:
        # CPU: read directly from /proc/stat via our own private delta
        # tracker (see module docstring) instead of jetson.cpu, which is
        # subject to the jtop-service-side reset-estimation glitch.
        cpu_percent, per_core = self._cpu_reader.read()

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


# Fields summarized (avg/min/max) for a test window and written into both
# the per-test CSV and the JSONL summary stats. Keep this list in sync with
# SysSnapshot's numeric fields that are meaningful to aggregate (fan_percent/
# fan_rpm and temp_c are nullable so they're handled specially below).
_NUMERIC_FIELDS = ["cpu_percent", "gpu_percent", "ram_percent", "swap_percent"]
_NULLABLE_FIELDS = ["fan_rpm", "temp_c"]


class SysRecorder:
    """Collects a de-duplicated series of SysSnapshots from a SysMonitor over
    the course of a test, for later CSV export + avg/min/max summarization.

    jtop publishes updates at its own cadence (SysMonitor polls at
    SYS_POLL_HZ, typically 2Hz) which is far slower than the 1kHz current
    sampling -- callers should call `poll()` from whatever loop they're
    already using to watch the clock/drain current samples (e.g. once per
    ~10-100ms), and this recorder only records a new row when the
    underlying snapshot's timestamp actually changes.
    """

    def __init__(self, sysmon: SysMonitor):
        self.sysmon = sysmon
        self._samples: list[SysSnapshot] = []
        self._last_t_seen: float = 0.0

    def start(self) -> None:
        self._samples = []
        self._last_t_seen = 0.0

    def poll(self) -> None:
        snap = self.sysmon.latest
        if snap.ok and snap.t > self._last_t_seen:
            self._last_t_seen = snap.t
            self._samples.append(snap)

    def samples(self) -> list[SysSnapshot]:
        return list(self._samples)

    def summary_stats(self) -> dict:
        """Returns a flat dict of avg_/min_/max_<field> for each numeric
        field, plus counts. Nullable fields (fan_rpm, temp_c) are summarized
        over only the samples where they were actually reported."""
        samples = self._samples
        out: dict = {"sys_n_samples": len(samples)}
        if not samples:
            return out
        for field_name in _NUMERIC_FIELDS:
            values = [getattr(s, field_name) for s in samples]
            out[f"sys_avg_{field_name}"] = sum(values) / len(values)
            out[f"sys_min_{field_name}"] = min(values)
            out[f"sys_max_{field_name}"] = max(values)
        for field_name in _NULLABLE_FIELDS:
            values = [getattr(s, field_name) for s in samples if getattr(s, field_name) is not None]
            if values:
                out[f"sys_avg_{field_name}"] = sum(values) / len(values)
                out[f"sys_min_{field_name}"] = min(values)
                out[f"sys_max_{field_name}"] = max(values)
            else:
                out[f"sys_avg_{field_name}"] = None
                out[f"sys_min_{field_name}"] = None
                out[f"sys_max_{field_name}"] = None
        return out
