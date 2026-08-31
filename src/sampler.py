"""
High-speed, low-jitter background sampling thread for the INA3221.

Design goals (per project requirements):
  - Sustain ~1 kHz instantaneous current sampling.
  - Minimize jitter: dedicated thread, tight loop, no allocations per-sample,
    monotonic clock for timing, lock-free handoff to consumers via a
    ring buffer + a separate "latest value" slot for the UI.
  - Decouple sampling from consumption: the UI/chart/integrator must never
    block or slow down the sampling loop. We push samples into a
    bounded ring buffer under a lock only for the *consumer* side; the
    producer (sampling loop) writes without waiting on any consumer.

Threading/jitter notes:
  - Python's GIL means true real-time guarantees aren't possible, but we
    minimize per-iteration overhead: no dict lookups in hot path beyond
    what's unavoidable, no logging, no exceptions in steady state, and (see
    "ring buffer" note below) no per-sample heap allocation in the hot loop.
  - We use time.perf_counter() (monotonic, high resolution) for all timing
    and jitter measurement.
  - CRITICAL: CPython's default GIL switch interval is 5ms
    (sys.getswitchinterval()). When another thread (e.g. Textual's UI/event
    loop) is CPU-busy, the interpreter only considers handing the GIL back
    to a waiting thread roughly every 5ms -- coarser than the 1ms period
    needed for 1kHz sampling, and *far* coarser than needed at low target
    rates too, since a starved thread that finally gets the GIL still has
    to catch up on whatever periods it missed. This was measured to cause
    the achieved rate to lag the target by 10-20% even at 100Hz once a
    Textual app with live widgets was running alongside the sampler.
    Importing this module calls `sys.setswitchinterval(0.0001)` (100us) as a
    process-wide fix -- this is a global interpreter setting (not
    per-thread), harmless to set repeatedly/from multiple call sites, and
    resolves the lag at every rate tested (100/200/500/1000Hz) even with
    the full sparkline UI active. The tradeoff is slightly more GIL-switch
    overhead system-wide, which is negligible compared to the accuracy win
    for this use case.
  - CPU-reduction pass (see project plan, "reduce sampler CPU footprint"):
    the sampling loop previously spent much of its CPU budget in a Python
    busy-spin ("while now < next_deadline: check again") for the final
    ~500us before each deadline, because time.sleep()'s real-world
    wake-up granularity on this non-RT kernel was too imprecise to trust
    for anything shorter. We now sleep via clock_nanosleep(CLOCK_MONOTONIC,
    TIMER_ABSTIME, ...) through ctypes instead (see `_abs_nanosleep_until`
    below), which sleeps to an *absolute* monotonic deadline with much
    better real-world precision than a relative time.sleep() on Linux, so
    the busy-spin window could be shrunk substantially (see
    SPIN_THRESHOLD_S) without reintroducing the achieved-rate lag. Falls
    back to the original time.sleep()-based spin loop if clock_nanosleep
    isn't available (e.g. non-Linux dev environment) so the module still
    works everywhere, just with the old (higher-CPU) timing behavior.
"""
from __future__ import annotations

import array
import ctypes
import ctypes.util
import itertools
import sys
import threading
import time
from dataclasses import dataclass, field

from ina3221 import INA3221

# See "CRITICAL" note above: tightens CPython's GIL handoff granularity so
# a busy UI thread can't starve the sampler thread past our target period.
# Process-wide and idempotent; safe to import this module without ever
# constructing a Sampler and still get the benefit for any other threads.
sys.setswitchinterval(0.0001)


# --- Absolute-deadline nanosleep via libc, with graceful fallback ----------
# On Linux, clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...) sleeps until
# an absolute point on the monotonic clock, which avoids the extra scheduling
# latency a *relative* sleep (time.sleep()) accumulates between "compute how
# long to sleep" and "actually start sleeping". This lets the sampling loop
# sleep much closer to its deadline before falling back to a tight spin,
# cutting CPU usage without reintroducing the achieved-rate lag that a
# coarser sleep would cause.

CLOCK_MONOTONIC = 1
TIMER_ABSTIME = 1


class _timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


def _load_clock_nanosleep():
    """Returns a callable(deadline_perf_counter_seconds) -> None that sleeps
    until that absolute perf_counter()-timebase deadline, or None if
    clock_nanosleep isn't available on this platform (e.g. non-Linux)."""
    try:
        libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
        if not hasattr(libc, "clock_nanosleep"):
            return None
        libc.clock_nanosleep.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(_timespec),
            ctypes.POINTER(_timespec),
        ]
        libc.clock_nanosleep.restype = ctypes.c_int
    except (OSError, AttributeError):
        return None

    # time.perf_counter() and CLOCK_MONOTONIC are not guaranteed to share an
    # epoch, so we anchor once at import time and convert deadlines expressed
    # in perf_counter() seconds into CLOCK_MONOTONIC seconds via a fixed
    # offset. Good enough for our purposes (short relative sleeps); we are
    # not trying to sleep to an absolute wall-clock time.
    ts_now = _timespec()
    ret = libc.clock_gettime(CLOCK_MONOTONIC, ctypes.byref(ts_now))
    if ret != 0:
        return None
    monotonic_now = ts_now.tv_sec + ts_now.tv_nsec * 1e-9
    perf_now = time.perf_counter()
    offset = monotonic_now - perf_now

    def sleep_until(deadline_perf_s: float) -> None:
        target = deadline_perf_s + offset
        ts = _timespec(tv_sec=int(target), tv_nsec=int((target % 1.0) * 1e9))
        # Ignore return value: on EINTR/etc we just fall through to the
        # caller's own re-check-the-clock loop, which handles early wakeups
        # (or effectively no-ops) fine either way.
        libc.clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ctypes.byref(ts), None)

    return sleep_until


_clock_nanosleep_until = _load_clock_nanosleep()

# How close to the deadline (in seconds) we switch from sleeping to a tight
# busy-spin. With time.sleep() this needed to stay large (500us) because a
# relative sleep on a non-RT kernel can wake up several ms late. With
# clock_nanosleep's absolute-deadline sleep, wake-up precision is much
# tighter in practice, so we can shrink this substantially and spend far
# less wall-clock time (and CPU) spinning. Falls back to the original,
# larger threshold when clock_nanosleep isn't available.
SPIN_THRESHOLD_S = 0.00005 if _clock_nanosleep_until is not None else 0.0005


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

    Storage: samples are held in a preallocated ring buffer of two parallel
    `array.array('d', ...)` arrays (timestamps and mA values) rather than a
    `collections.deque` of per-sample `Sample` objects. This removes all
    per-sample heap allocation from the hot sampling loop -- at 1000Hz that
    was ~1000 small-object allocations/sec, adding avoidable GC pressure and
    per-object overhead on top of the I2C transaction itself. `Sample`
    objects are only constructed on the *consumer* side (`drain()` /
    `peek_recent()`), which run far less often (UI tick rate, or once at
    the end of a test) -- so downstream code (tests.py, csv_log.py, app.py)
    is unaffected and still works with lists of `Sample` objects.

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
        self._capacity = buffer_maxlen
        # Ring buffer storage: preallocated, fixed-size, no per-sample
        # allocation on the write path. `_buf_t`/`_buf_ma` are written at
        # index (write_count % capacity); `_write_count` is a monotonically
        # increasing total-samples-ever-written counter (not wrapped), so
        # readers can compute exactly which slots are valid/unread.
        self._buf_t = array.array("d", [0.0]) * buffer_maxlen
        self._buf_ma = array.array("d", [0.0]) * buffer_maxlen
        self._write_count = 0
        self._read_count = 0  # samples consumed so far via drain()
        self._lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self.latest: Sample | None = None
        self._stats = SamplerStats(target_hz=target_hz)
        self._period_window: array.array = array.array("d", [0.0]) * stats_window
        self._period_window_len = stats_window
        self._period_write_idx = 0
        self._period_count = 0  # how many entries are valid (caps at stats_window)

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
        buf_t = self._buf_t
        buf_ma = self._buf_ma
        capacity = self._capacity
        period_window = self._period_window
        period_window_len = self._period_window_len
        lock = self._lock
        sleep_until = _clock_nanosleep_until
        spin_threshold = SPIN_THRESHOLD_S

        next_deadline = perf_counter()
        last_t = next_deadline
        n = 0
        try:
            while not self._stop_event.is_set():
                now = perf_counter()
                if now < next_deadline:
                    remaining = next_deadline - now
                    if remaining > spin_threshold:
                        if sleep_until is not None:
                            # Absolute-deadline sleep: precise enough to
                            # leave only spin_threshold's worth of spin.
                            sleep_until(next_deadline - spin_threshold)
                        else:
                            # Fallback: relative time.sleep(), conservative
                            # margin retained (matches the original design).
                            time.sleep(max(0.0, remaining - 0.0003))
                    continue

                uv = read(channel)
                current_ma = (uv * 1e-6 / shunt_ohms) * 1000.0
                t = perf_counter()

                # Write into the ring buffer -- no allocation, just two
                # array element assignments -- then bump the write counter.
                idx = self._write_count % capacity
                with lock:
                    buf_t[idx] = t
                    buf_ma[idx] = current_ma
                    self._write_count += 1
                # `latest` still uses a small Sample object -- this is one
                # allocation per sample, same as before, kept for API
                # compatibility with consumers that read `sampler.latest`
                # directly (app.py's UI tick, dev_last_voltage-adjacent
                # code). This is far cheaper than the deque-append path it
                # replaces and is not the dominant remaining cost; left
                # as-is to avoid widening the change's blast radius.
                self.latest = Sample(t=t, current_ma=current_ma)

                period_window[self._period_write_idx] = t - last_t
                self._period_write_idx = (self._period_write_idx + 1) % period_window_len
                if self._period_count < period_window_len:
                    self._period_count += 1
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
        """Pop all buffered samples since the last drain (consumer-side).
        Constructs `Sample` objects here (consumer side, low frequency)
        rather than in the hot sampling loop."""
        with self._lock:
            start = self._read_count
            end = self._write_count
            if end - start > self._capacity:
                # Reader fell behind by more than a full buffer's worth
                # (e.g. a long-idle UI); only the newest `capacity` samples
                # still exist in the ring buffer, so clamp instead of
                # returning stale/overwritten slots.
                start = end - self._capacity
            self._read_count = end
            buf_t = self._buf_t
            buf_ma = self._buf_ma
            capacity = self._capacity
            out = [
                Sample(t=buf_t[i % capacity], current_ma=buf_ma[i % capacity])
                for i in range(start, end)
            ]
        return out

    def peek_recent(self, n: int) -> list[Sample]:
        """Non-destructive peek at the last n buffered samples (for charting).

        O(n): reads directly from the ring buffer without disturbing the
        drain-side read position, so this can be called freely from a UI
        loop without interfering with a test's own drain()-based bookkeeping.
        """
        with self._lock:
            end = self._write_count
            available = min(end, self._capacity)
            count = min(n, available)
            start = end - count
            buf_t = self._buf_t
            buf_ma = self._buf_ma
            capacity = self._capacity
            out = [
                Sample(t=buf_t[i % capacity], current_ma=buf_ma[i % capacity])
                for i in range(start, end)
            ]
        return out

    def stats(self) -> SamplerStats:
        s = self._stats
        count = self._period_count
        if count:
            window = self._period_window
            # Only the first `count` entries are valid before the ring
            # buffer has wrapped once; after that all `period_window_len`
            # entries are valid (oldest gets overwritten in place).
            valid = window[:count] if count < self._period_window_len else window
            mean_p = sum(valid) / count
            s.achieved_hz = 1.0 / mean_p if mean_p > 0 else 0.0
            s.mean_period_us = mean_p * 1e6
            s.max_period_us = max(valid) * 1e6
            var = sum((p - mean_p) ** 2 for p in valid) / count
            s.jitter_std_us = (var ** 0.5) * 1e6
        return s
