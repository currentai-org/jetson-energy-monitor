"""
Test routines built on top of the Sampler:
  - BaselineTest: average idle current over a fixed window (default 10s).
  - EnergyTest: start/stop-triggered capture that integrates current over
    time (trapezoidal rule) to compute mWh and mAh consumed, plus summary
    stats (mean/min/max current, duration, sample count, achieved rate).
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

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
    return TestResult(
        name="Baseline (idle) power test",
        started_at=start,
        duration_s=dur,
        n_samples=len(samples),
        mean_current_ma=mean_ma,
        min_current_ma=min_ma,
        max_current_ma=max_ma,
        achieved_hz=achieved_hz,
        bus_voltage_v=bus_v,
    )


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
        return TestResult(
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
        )
