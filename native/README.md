# native/ -- max-efficiency sampler research spike (branch: native-sampler-spike)

Context: even with the Textual TUI removed (`--headless` CLI mode), the
user still observed 10-15% CPU utilization / load average ~2.0 on a
different Jetson device, though the achieved sample rate is dead-on
1kHz. This branch investigates how much further CPU footprint can be
reduced, starting from a pure-C reference point before considering a
Cython/C-extension approach that could be called from Python.

## ina_bench.c

A **standalone benchmark**, not wired into the app. Deliberately minimal
scope per the spike's goal: just read the INA3221 over raw I2C ioctl,
queue samples in a preallocated ring buffer, and periodically flush a
batch to a CSV file with a single buffered `fwrite()` rather than one
`fprintf()` per sample. Uses `clock_nanosleep(CLOCK_MONOTONIC,
TIMER_ABSTIME, ...)` for absolute-deadline pacing, mirroring the
technique `sampler.py` already uses via ctypes -- now native, no
ctypes/ FFI overhead, no GIL.

Build:
```bash
cd native
gcc -O2 -Wall -o ina_bench ina_bench.c -lm
```

Run (same permissions as the Python driver -- no sudo needed if your user
is in the `i2c` group):
```bash
./ina_bench --hz 1000 --duration 10 --channel 1 --out /tmp/ina_bench.csv
```

Full flag list:
```bash
./ina_bench --help
```

While it runs, measure CPU/load in another terminal the same way you'd
check the Python headless mode, e.g.:
```bash
ps -o %cpu,rss,etime -p $(pgrep ina_bench)
uptime
```

Compare against the Python headless CLI's achieved Hz / CPU%/load-average
figures for an apples-to-apples reference point. This number is the
practical floor for "how much can pure I/O + timing + batched CSV writes
cost on this hardware" -- if a compiled C program can't get meaningfully
lower than the headless Python CLI's Sampler thread, that's a strong
signal Python isn't the actual bottleneck (kernel I2C driver overhead,
context-switch cost from waking a thread ~1000x/sec, etc. would then be
the next thing to profile instead of jumping to Cython).

## Next steps (not yet done)

- Benchmark on the "different Jetson" the user reported the 10-15%/load
  ~2.0 regression on, not just pocket-infer-6a8f, since the two devices
  may have different I2C driver/kernel versions.
- If ina_bench's CPU% is meaningfully lower than the Python headless
  mode's, that's the signal to invest in a Cython (or ctypes-callable .so)
  extension for the hot sampling loop specifically -- not the whole app.
- If ina_bench's CPU% is close to the Python headless mode's, the
  bottleneck is more likely kernel-side (I2C driver/interrupt overhead,
  scheduler wakeup cost) and no amount of Cython in userspace will help;
  worth checking `i2cdetect`/kernel I2C driver clock-stretching settings,
  or whether polling less aggressively via on-chip AVG (hardware
  averaging register) rather than software-side 1kHz polling could hit
  similar mAh-integration accuracy for much less wakeup overhead.

## Benchmark results (pocket-infer-6a8f, 2026-09-02)

Measured via a small `wait4()`/`getrusage()`-based wrapper (total process
CPU time = `ru_utime + ru_stime`, summed across the whole run -- more
precise than reading instantaneous `%CPU` off `ps`, which is too coarse
at these levels), 1000 Hz target, channel 1 (VDD_IN), 10s runs, clean
otherwise-idle device (`uptime` load average <0.2 throughout):

| implementation                              | total CPU time / 10s run | ~% of one core |
|----------------------------------------------|---------------------------|-----------------|
| `ina_bench` (this file, pure C)              | 0.54 - 0.55 s              | **~5.4%**       |
| `--headless baseline` (Python, no Textual)   | 1.93 - 1.97 s              | **~19.5%**      |

Both hit the 1000 Hz target rate essentially exactly (achieved rate
998.5-1000.0 Hz in both cases; the earlier Python-only investigation
already established the achieved rate itself isn't the problem -- CPU
footprint is). The pure-C reference point is **roughly 3.5x cheaper**
than the current best-case Python path (`--headless`, no Textual, no
sparklines) at the same target rate on this device.

This confirms there's real headroom below the Python floor -- the
10-15%/load~2.0 the user is seeing on a different Jetson is plausibly
Python/CPython overhead (per-sample I2C wrapper call overhead via
smbus2's `i2c_rdwr` message construction, `Sample` dataclass allocation
on the `.latest` path, GIL bookkeeping even with only one active thread,
etc.), not an unavoidable kernel/hardware floor. This makes a case for
the Cython/C-extension route as the next step, rather than concluding
Python's overhead here is already close to the hardware limit.

Caveat: this was measured on `pocket-infer-6a8f` only so far, which was
NOT the device the user reported the regression on -- per "Next steps"
above, re-running this same comparison on the actual affected device is
the immediate next step before committing to a Cython implementation.
