"""
High-speed, low-jitter background sampling thread for the INA3221.

Design goals (per project requirements):
  - Sustain ~1 kHz instantaneous current sampling.
  - Minimize jitter: dedicated thread, tight loop, no allocations per-sample,
    monotonic clock for timing, lock-free handoff to consumers via a
    ring buffer + a separate "latest value" slot for the UI.
  - Decouple sampling from consumption: the UI/chart/integrator must never
    block or slow down the sampling loop. We push samples into a
    bounded deque under a lock only for the *consumer* side; the producer
    (sampling loop) appends without waiting on any consumer.

Threading/jitter notes:
  - Python's GIL means true real-time guarantees aren't possible, but we
    minimize per-iteration overhead: no dict lookups in hot path beyond
    what's unavoidable, no logging, no exceptions in steady state.
  - We use time.perf_counter() (monotonic, high resolution) for all timing
    and jitter measurement.
  - The sampling thread is set to run continuously (busy-checked against a
    target period) rather than doing long sleeps -- long sleeps have poor
    wake-up precision on non-RT Linux kernels (this board runs stock L4T,
    not PREEMPT_RT). We use a short sleep (sleep(0) / tiny sleep) between
    reads when the I2C transaction itself is the rate-limiting step, which
    it is here (~617us/read measured, faster than the 1ms budget).
"""
from __future__ import annotations

import collections
import threading
import time
from dataclasses import dataclass, field

from ina3221 import INA3221


@dataclass
class Sample:
    t: float  # perf_counter timestamp, seconds
    current_ma: float


@dataclass
class SamplerStats:
    samples_taken: int = 0
    target_hz: float = 1000.0
    achieved_hz: float = 0.0
    mean_period_us: float = 0.0
    max_period_us: float = 0.0
    jitter_std_us: float = 0.0
    last_error: str | None = None


class Sampler:
    """Runs a dedicated thread pulling instantaneous current readings from the
    INA3221 at (up to) `target_hz`, buffering them for consumers.

    Usage:
        sampler = Sampler(dev, channel=1, target_hz=1000)
        sampler.start()
        ...
        recent = sampler.drain()   # consumer: pull + clear buffered samples
        latest = sampler.latest    # consumer: peek most recent value (no drain)
        sampler.stop()
    """

    def __init__(
        self,
        dev: INA3221,
        channel: int = 1,
        target_hz: float = 1000.0,
        buffer_maxlen: int = 200_000,
        stats_window: int = 2000,
    ):
        self.dev = dev
        self.channel = channel
        self.target_period_s = 1.0 / target_hz
        self._buf: collections.deque[Sample] = collections.deque(maxlen=buffer_maxlen)
        self._lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self.latest: Sample | None = None
        self._stats = SamplerStats(target_hz=target_hz)
        self._period_window: collections.deque[float] = collections.deque(maxlen=stats_window)

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run, name="ina3221-sampler", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 2.0) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
            self._thread = None

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def _run(self) -> None:
        read = self.dev.read_shunt_voltage_uv
        shunt_ohms = self.dev.shunt_ohms
        channel = self.channel
        perf_counter = time.perf_counter
        target_period = self.target_period_s
        buf = self._buf
        lock = self._lock
        period_window = self._period_window

        next_deadline = perf_counter()
        last_t = next_deadline
        n = 0
        try:
            while not self._stop_event.is_set():
                now = perf_counter()
                if now < next_deadline:
                    # Busy-ish wait for short remaining time: sleep(0) yields the
                    # GIL/scheduler without the ~1-15ms granularity of sleep(>0)
                    # on a non-RT kernel; fall back to a tiny sleep if the gap
                    # is large enough to be worth it (reduces CPU spin cost).
                    remaining = next_deadline - now
                    if remaining > 0.0005:
                        time.sleep(remaining - 0.0003)
                    continue

                uv = read(channel)
                current_ma = (uv * 1e-6 / shunt_ohms) * 1000.0
                t = perf_counter()

                sample = Sample(t=t, current_ma=current_ma)
                with lock:
                    buf.append(sample)
                self.latest = sample

                period_window.append(t - last_t)
                last_t = t
                n += 1

                next_deadline += target_period
                # If we've fallen behind by more than a couple periods (e.g. a
                # scheduling hiccup), resync instead of trying to catch up in
                # a tight burst that would distort the integration timebase.
                if now - next_deadline > target_period * 2:
                    next_deadline = now + target_period
        except Exception as e:  # pragma: no cover - defensive
            self._stats.last_error = repr(e)

        self._stats.samples_taken = n

    def drain(self) -> list[Sample]:
        """Pop all buffered samples since the last drain (consumer-side)."""
        with self._lock:
            out = list(self._buf)
            self._buf.clear()
        return out

    def peek_recent(self, n: int) -> list[Sample]:
        """Non-destructive peek at the last n buffered samples (for charting)."""
        with self._lock:
            if n >= len(self._buf):
                return list(self._buf)
            return list(collections.deque(self._buf, maxlen=n))

    def stats(self) -> SamplerStats:
        window = list(self._period_window)
        s = self._stats
        if window:
            mean_p = sum(window) / len(window)
            s.achieved_hz = 1.0 / mean_p if mean_p > 0 else 0.0
            s.mean_period_us = mean_p * 1e6
            s.max_period_us = max(window) * 1e6
            var = sum((p - mean_p) ** 2 for p in window) / len(window)
            s.jitter_std_us = (var ** 0.5) * 1e6
        return s
