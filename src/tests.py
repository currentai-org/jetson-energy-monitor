"""
Test routines built on top of the Sampler:
  - BaselineTest: average idle current over a fixed window (default 10s).
  - EnergyTest: start/stop-triggered capture that integrates current over
    time (trapezoidal rule) to compute mWh and mAh consumed, plus summary
    stats (mean/min/max current, duration, sample count, achieved rate).
"""
from __future__ import annotations

import socket
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from csv_log import write_samples_csv
from jsonl_log import append_result
from sampler import Sample, Sampler


@dataclass
class TestResult:
    name: str
    started_at: float
    duration_s: float
    n_samples: int
    mean_current_ma: float
    min_current_ma: float
    max_current_ma: float
    achieved_hz: float
    bus_voltage_v: float | None = None
    mwh: float | None = None
    mah: float | None = None
    csv_path: Path | None = None
    jsonl_path: Path | None = None

    def summary_lines(self) -> list[str]:
        lines = [
            f"{self.name}",
            f"  duration:      {self.duration_s:.3f} s",
            f"  samples:       {self.n_samples}  (~{self.achieved_hz:.0f} Hz)",
            f"  mean current:  {self.mean_current_ma:.1f} mA",
            f"  min / max:     {self.min_current_ma:.1f} / {self.max_current_ma:.1f} mA",
        ]
        if self.bus_voltage_v is not None:
            lines.append(f"  bus voltage:   {self.bus_voltage_v:.3f} V")
        if self.mwh is not None:
            lines.append(f"  energy:        {self.mwh:.3f} mWh  ({self.mah:.3f} mAh)")
        if self.csv_path is not None:
            lines.append(f"  raw samples:   {self.csv_path}")
        if self.jsonl_path is not None:
            lines.append(f"  logged to:     {self.jsonl_path}")
        return lines


def _stats_from_samples(samples: list[Sample]) -> tuple[float, float, float, float]:
    """Returns (mean_ma, min_ma, max_ma, duration_s)."""
    if not samples:
        return 0.0, 0.0, 0.0, 0.0
    currents = [s.current_ma for s in samples]
    duration = samples[-1].t - samples[0].t
    return (sum(currents) / len(currents), min(currents), max(currents), duration)


def integrate_mwh(samples: list[Sample], bus_voltage_v: float) -> tuple[float, float]:
    """Trapezoidal integration of current over time -> (mAh, mWh)."""
    if len(samples) < 2:
        return 0.0, 0.0
    mah = 0.0
    for i in range(1, len(samples)):
        dt_h = (samples[i].t - samples[i - 1].t) / 3600.0
        avg_ma = (samples[i].current_ma + samples[i - 1].current_ma) / 2.0
        mah += avg_ma * dt_h
    mwh = mah * bus_voltage_v
    return mah, mwh


def _log_result_jsonl(
    test_type: str,
    result: TestResult,
    sampler: Sampler,
    dev,
    extra: dict | None = None,
) -> Path:
    """Builds a rich JSON record for this test run and appends it to the
    shared results.jsonl in the cache dir. Includes everything from
    TestResult, plus sampler jitter/rate stats, device/config context, and
    a wall-clock ISO timestamp (perf_counter() values in TestResult aren't
    meaningful across process runs, so we also capture time.time()-based
    fields for cross-run comparison).
    """
    stats = sampler.stats()
    now_wall = time.time()
    record: dict = {
        "test_type": test_type,
        "logged_at": now_wall,
        "logged_at_iso": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now_wall)),
        "hostname": socket.gethostname(),
        # --- TestResult fields ---
        "name": result.name,
        "duration_s": result.duration_s,
        "n_samples": result.n_samples,
        "mean_current_ma": result.mean_current_ma,
        "min_current_ma": result.min_current_ma,
        "max_current_ma": result.max_current_ma,
        "achieved_hz": result.achieved_hz,
        "bus_voltage_v": result.bus_voltage_v,
        "mwh": result.mwh,
        "mah": result.mah,
        "csv_path": str(result.csv_path) if result.csv_path else None,
        # --- sampler/device context (useful for comparing runs with
        # different configs, or diagnosing a run with unusually high jitter) ---
        "sampler_target_hz": sampler.target_period_s and (1.0 / sampler.target_period_s),
        "sampler_mean_period_us": stats.mean_period_us,
        "sampler_max_period_us": stats.max_period_us,
        "sampler_jitter_std_us": stats.jitter_std_us,
        "sampler_last_error": stats.last_error,
        "i2c_channel": sampler.channel,
        "shunt_ohms": dev.shunt_ohms,
        "i2c_address": hex(dev.address),
    }
    if extra:
        record.update(extra)
    return append_result(record)


def run_baseline_test(sampler: Sampler, dev, duration_s: float = 10.0) -> TestResult:
    """Blocking-ish baseline capture: assumes sampler is already running
    continuously; this just watches the clock and collects what accumulates.
    Call from a worker thread/async task, not the UI thread, if using Textual.
    """
    sampler.drain()  # clear stale samples so we only measure this window
    start = time.perf_counter()
    while time.perf_counter() - start < duration_s:
        time.sleep(0.01)
    samples = sampler.drain()
    mean_ma, min_ma, max_ma, dur = _stats_from_samples(samples)
    achieved_hz = (len(samples) / dur) if dur > 0 else 0.0
    bus_v = dev.read_bus_voltage_v(sampler.channel)
    csv_path = write_samples_csv("baseline", samples, bus_voltage_v=bus_v) if samples else None
    result = TestResult(
        name="Baseline (idle) power test",
        started_at=start,
        duration_s=dur,
        n_samples=len(samples),
        mean_current_ma=mean_ma,
        min_current_ma=min_ma,
        max_current_ma=max_ma,
        achieved_hz=achieved_hz,
        bus_voltage_v=bus_v,
        csv_path=csv_path,
    )
    result.jsonl_path = _log_result_jsonl(
        "baseline", result, sampler, dev, extra={"requested_duration_s": duration_s}
    )
    return result


class EnergyCapture:
    """Spacebar-toggled start/stop energy capture session."""

    def __init__(self, sampler: Sampler, dev):
        self.sampler = sampler
        self.dev = dev
        self.active = False
        self._samples: list[Sample] = []

    def start(self) -> None:
        self.sampler.drain()
        self._samples = []
        self.active = True

    def poll(self) -> None:
        """Call periodically (e.g. from UI refresh) while active to accumulate
        drained samples -- keeps the sampler's internal buffer from growing
        unbounded during a long capture."""
        if self.active:
            self._samples.extend(self.sampler.drain())

    def stop(self) -> TestResult:
        self.active = False
        self._samples.extend(self.sampler.drain())
        samples = self._samples
        mean_ma, min_ma, max_ma, dur = _stats_from_samples(samples)
        achieved_hz = (len(samples) / dur) if dur > 0 else 0.0
        bus_v = self.dev.read_bus_voltage_v(self.sampler.channel)
        mah, mwh = integrate_mwh(samples, bus_v)
        csv_path = write_samples_csv("energy", samples, bus_voltage_v=bus_v) if samples else None
        result = TestResult(
            name="Energy usage test",
            started_at=samples[0].t if samples else time.perf_counter(),
            duration_s=dur,
            n_samples=len(samples),
            mean_current_ma=mean_ma,
            min_current_ma=min_ma,
            max_current_ma=max_ma,
            achieved_hz=achieved_hz,
            bus_voltage_v=bus_v,
            mwh=mwh,
            mah=mah,
            csv_path=csv_path,
        )
        result.jsonl_path = _log_result_jsonl("energy", result, self.sampler, self.dev)
        return result
