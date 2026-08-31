# Jetson Energy Usage

A Python TUI for running energy-efficiency tests on the NVIDIA Jetson Orin
Nano Developer Kit, using its **onboard INA3221** current/voltage monitor
(I2C address `0x40`, bus 1) — no external hardware required.

## Why direct register access instead of the kernel `hwmon` driver?

The stock L4T kernel driver exposes the INA3221 via
`/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon1/`, but on this board it's
configured to round-robin all 3 channels with a slow bus-voltage conversion
time (~4.156 ms), giving an aggregate update rate of only ~77 Hz — and the
config isn't writable without root (`samples` / conversion-time attributes
are root-owned).

Instead, `src/ina3221.py` talks to the chip directly over `/dev/i2c-1` via
`smbus2`, reconfiguring it for single-channel (VDD_IN / total board input
power), minimum conversion time (140 µs), no on-chip averaging. This needs
no `sudo` — the `ubuntu` user is already in the `i2c` group, which owns
`/dev/i2c-1`.

Measured on `pocket-infer-6a8f` (Orin Nano Super):
- Single-channel shunt-voltage-only reads: **~1620 Hz** sustained (617 µs/read)
- Comfortably clears the 1 kHz target with headroom for jitter.

The original chip configuration is snapshotted on startup and restored on
exit (best-effort — see `INA3221.restore_config()`), so other tools reading
the standard hwmon path (e.g. `jtop`/`tegrastats`) return to their prior
behavior after this tool exits.

## Architecture

- **`src/ina3221.py`** — raw INA3221 register driver (config word encode/decode,
  shunt/bus voltage register reads, single-channel fast-mode helper).
- **`src/sampler.py`** — dedicated background thread pulling current readings
  at a target rate (default 1000 Hz) into a bounded ring buffer. Uses
  `time.perf_counter()` for all timing, a hybrid sleep/spin wait to minimize
  jitter on a non-RT kernel, and tracks achieved rate / jitter stats. The
  sampling loop never blocks on UI/consumer code.
- **`src/tests.py`** — test logic built on the sampler:
  - `run_baseline_test`: averages current over a fixed window (default 10s).
  - `EnergyCapture`: start/stop-triggered capture; integrates current over
    time via the trapezoidal rule to compute mAh/mWh consumed.
- **`src/csv_log.py`** — every test run's raw per-sample data (elapsed time,
  instantaneous current, estimated instantaneous power) is written to a
  timestamped CSV in the user cache directory (`$XDG_CACHE_HOME/jetson-energy-usage`,
  falling back to `~/.cache/jetson-energy-usage`) so runs can be reviewed or
  replotted later. Filenames: `YYYYMMDD_HHMMSS_<baseline|energy>.csv`. The
  written path is echoed in the TUI's result log and in `TestResult.csv_path`.
  A second, paired CSV -- `YYYYMMDD_HHMMSS_<baseline|energy>_sysinfo.csv` --
  captures one row per system-resource snapshot taken during the same test
  window (CPU%, GPU%, RAM/swap used+total+%, fan RPM/duty, die temp); see
  `write_sysinfo_csv`. Written only if system monitoring is available for
  that run (`TestResult.sysinfo_csv_path` is `None` otherwise).
- **`src/jsonl_log.py`** — every test run also appends one JSON object (one
  line) to a single running log, `results.jsonl`, in the same cache
  directory. One `b` invocation writes exactly one line; one complete `e`
  start/stop capture writes exactly one line. Different test types can
  carry different fields (e.g. only the energy test has `mwh`/`mah`; only
  the baseline test has `requested_duration_s`) -- consumers should key off
  `test_type` rather than assuming a fixed schema. Each record also carries
  sampler jitter/rate stats, device config (channel, shunt resistance, I2C
  address), and -- when system monitoring is available -- `sys_avg_/sys_min_/
  sys_max_<field>` summary stats (CPU%, GPU%, RAM%, swap%, fan RPM, die
  temp) aggregated over the test window, plus `sys_n_samples` and
  `sysinfo_csv_path`. Built via `tests._log_result_jsonl()`, called from
  both `run_baseline_test` and `EnergyCapture.stop`.
- **`src/app.py`** — the Textual TUI: a status bar (latest reading, achieved
  sample rate, jitter), a system-resource status bar (CPU, GPU, RAM, swap,
  fan, die temperature), three single-row unicode-block sparklines (current
  mA, CPU%, GPU%) in the jtop/jetson-stats visual style, a scrolling
  test-result log, and keybindings. UI refresh rate is a deliberately
  modest 5 Hz -- this is a "rough picture" tool, not an oscilloscope, and a
  higher refresh rate mostly just burns CPU on redraws without adding real
  information (the current-sampling rate for tests/CSVs/JSONL is completely
  independent of and unaffected by the UI refresh rate).
- **`src/sysinfo.py`** — background thread polling `jetson-stats` (jtop) at
  2 Hz for CPU load (aggregate + per-core), GPU load, RAM/swap usage, fan
  RPM/duty, and die temperature (thermal-junction zone). Runs independently
  of the INA3221 sampler so a slow/stalled jtop connection can never add
  jitter to current sampling. Degrades gracefully (`SysSnapshot(ok=False)`)
  if the jtop service isn't reachable or the client library version
  mismatches the running service. Also provides `SysRecorder`, which
  collects a de-duplicated snapshot series during a test window and
  produces avg/min/max summary stats (`summary_stats()`) for the JSONL log.

## Keybindings

| Key     | Action                                                              |
|---------|----------------------------------------------------------------------|
| `b`     | Run a **baseline (idle) power test** — averages current over 10s.   |
| `e`     | **Arm** the energy-usage test (informational; space works either way). |
| `space` | **Start/stop** an energy capture. On stop, integrates current × time (trapezoidal rule) over the window to report mWh / mAh consumed, plus mean/min/max current and achieved sample rate. |
| `r`     | Clear the live chart.                                                |
| `q`     | Quit (restores original INA3221 config).                            |

## Setup (on the Jetson)

```bash
cd ~/jetson-energy-usage
uv sync            # creates .venv, installs from pyproject.toml / uv.lock
uv run python src/app.py
```

Or via the plain requirements file (if not using `uv`):

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python src/app.py
```

### Command-line flags

```
usage: jetson-energy-usage [-h] [--sample-hz HZ] [--sysinfo-hz HZ] [--channel {1,2,3}] [--no-plots]

  --sample-hz HZ     Target INA3221 current sampling rate in Hz (default: 1000).
                      Measured achievable rate on pocket-infer-6a8f is ~1600 Hz
                      single-channel; requesting higher than the hardware/I2C bus
                      can sustain shows up as reduced 'Rate' and increased jitter
                      in the status bar rather than an error.
  --sysinfo-hz HZ     Poll rate in Hz for CPU/GPU/RAM/swap/fan/temp via jetson-stats
                      (jtop) (default: 2). jtop's own service publishes at roughly
                      1 Hz internally, so requesting much faster than that adds
                      polling overhead without more real data resolution.
  --channel {1,2,3}   INA3221 channel to sample (1=VDD_IN total board power,
                      2=VDD_CPU_GPU_CV, 3=VDD_SOC). Default: 1.
  --no-plots          Disable the live current and CPU/GPU sparklines entirely.
                      Status bars, keybindings, and CSV/JSONL logging are
                      unaffected -- only the Sparkline widgets are skipped.
                      Use this for the lowest possible UI overhead.
```

Example: `uv run python src/app.py --sample-hz 500 --sysinfo-hz 1` for a
lower-overhead run, or `uv run python src/app.py --no-plots` if the charts
themselves are the bottleneck.

No `sudo` needed as long as your user is in the `i2c` group:

```bash
groups   # should list "i2c"
```

## Performance history

An earlier version of this project used `textual-plotext` for the live
current/CPU/GPU charts. Two issues caused the UI to peg a CPU core and
effectively hang after ~15s of runtime:

1. **`Sampler.peek_recent(n)` was silently O(buffer size), not O(n).** It
   rebuilt a whole new `collections.deque(self._buf, maxlen=n)` from the
   source buffer on every call. Since the sample buffer isn't drained while
   idle (only tests drain it), it grows continuously (up to a 200k-sample
   cap at ~1kHz, ~200s of idle running), so a UI polling this 12x/sec at
   `n=15000` became dramatically slower over time. Fixed in `sampler.py` by
   walking only the needed elements via `itertools.islice(reversed(buf), n)`
   -- true O(n) regardless of how long the buffer has been accumulating.
2. **plotext's per-frame rendering of thousands of high-resolution data
   points** was expensive even after (1) was fixed. Replaced with Textual's
   built-in `Sparkline` widget (single-row unicode block characters, the
   same visual style jtop/jetson-stats uses) which renders in O(terminal
   width) per frame regardless of how much history is fed to it, and only
   ever retains a small fixed-length rolling window (`SPARKLINE_HISTORY`,
   120 points by default). `textual-plotext`/`plotext` are no longer
   dependencies.
3. **UI refresh rate reduced from 12 Hz to 5 Hz** by default -- this is a
   "rough picture" tool, not an oscilloscope, and the higher rate mostly
   burned CPU on redraws without adding real information (test data is
   still captured at the full sampler rate regardless of UI refresh rate).

If you still see high CPU usage after these fixes, note that a good chunk
of it is inherent to sustaining 1kHz I2C polling in Python on a non-RT
kernel (measured ~45-50% of one core for the sampler+sysmon threads alone,
independent of the UI) -- use `--sample-hz` to reduce the target rate, or
`--no-plots` to shave off the remaining sparkline overhead, if you need to
free up more CPU headroom.

## Achieved-rate lag fix (2026-08-31)

Independent of the CPU-usage fixes above, the **achieved sampling rate**
(shown in the status bar's "Rate:" field) used to consistently lag the
requested `--sample-hz` by 10-20%, even at low, easily-achievable rates
like 100Hz -- e.g. `--sample-hz 100` might show ~94-97Hz in practice. Root
cause: **CPython's default GIL switch interval is 5ms**
(`sys.getswitchinterval()`). When the Textual UI thread is CPU-busy
(rendering widgets, even simple ones), the interpreter only reconsiders
handing the GIL to the waiting sampler thread roughly every 5ms -- coarser
than the 1ms period needed for 1kHz sampling, and enough to compound into
a double-digit-percent shortfall at any target rate once a starved thread
has to catch up on missed periods.

Fix: `src/sampler.py` calls `sys.setswitchinterval(0.0001)` (100us) at
import time -- a process-wide interpreter setting, harmless to set
repeatedly, with negligible overhead compared to the accuracy gained.
Verified on `pocket-infer-6a8f`: achieved rate now matches target almost
exactly at 100/200/500/1000 Hz, both with `--no-plots` and with the full
sparkline UI active (998-1000Hz achieved at a 1000Hz target either way).

Note: this device is a **shared, multi-user Jetson** -- other users'
workloads (observed: an unrelated `llama-mtmd-cli` inference job) can and
will steal real CPU time from the sampler thread regardless of any
in-process fix. If you see achieved-rate lag that doesn't match the
pattern above, check `uptime`/`ps aux --sort=-%cpu` for competing load
before assuming it's a code regression.

## CPU-footprint reduction pass (2026-08-31)

The user's application shares this device with the sampler, so further
reducing the sampler's own CPU cost (beyond the achieved-rate fix above)
matters independent of accuracy. Four changes landed from a broader
options/effort tradeoff analysis (see `.hermes/plans/` for the full
options list, including options deferred for later):

1. **Absolute-deadline sleep via `clock_nanosleep(CLOCK_MONOTONIC,
   TIMER_ABSTIME, ...)`** (through `ctypes`, no new dependency) replaces the
   previous relative `time.sleep()` in the sampler's wait loop. The old
   design had to keep a large busy-spin window (500us) before each sample
   deadline because a relative sleep's real-world wake-up precision on this
   non-RT kernel was too imprecise to trust for anything shorter -- most of
   the sampler's CPU cost was this unproductive spin, not the actual I2C
   read. An absolute monotonic-clock deadline sleeps far more precisely, so
   the spin window shrank to 50us. Falls back to the original
   `time.sleep()`-based spin loop (larger threshold) if `clock_nanosleep`
   isn't available on the platform (e.g. non-Linux dev environment) --
   `sampler.py`'s `_load_clock_nanosleep()` probes for it at import time.
2. **Preallocated ring buffer (`array.array('d', ...)` x2) replaces the
   per-sample `Sample` dataclass + `collections.deque`** in the hot sampling
   loop. At 1kHz that removed ~1000 small-object heap allocations/sec from
   the producer thread; `Sample` objects are now only constructed on the
   consumer side (`drain()`/`peek_recent()`, called at UI-tick or
   end-of-test frequency, not per-sample). `drain()`/`peek_recent()` keep
   their original list-of-`Sample` return type, so `tests.py`, `csv_log.py`,
   and `app.py` needed no changes.
3. **Bus-voltage read on the UI thread throttled from every tick (5Hz) to
   `VOLTAGE_POLL_HZ` (1Hz default)**, with the cached value returned in
   between. This was a second blocking I2C transaction competing for the
   GIL against the sampler thread on every UI tick, for a value (bus
   voltage on a stiff supply) that doesn't meaningfully change 5x/sec.
4. **Shrunk the spin-to-sleep threshold in the sampler's wait loop** (see
   item 1) -- listed separately in the original options list but landed
   together with the `clock_nanosleep` change since they're two views of
   the same fix (better sleep precision enables a smaller spin window).

**Measured impact** (pocket-infer-6a8f, `/proc/<pid>/stat` utime+stime
delta over a 15s window, device otherwise idle -- always check
`uptime`/`ps aux --sort=-%cpu` before trusting a CPU measurement on this
shared device):

| target_hz | `--no-plots` before | `--no-plots` after | full sparkline UI after |
|-----------|---------------------|---------------------|--------------------------|
| 1000      | ~52-55%             | **25.2%**           | 51.8%                    |
| 500       | (not separately measured pre-fix) | 19.4% | 41.9%                    |
| 200       | (not separately measured pre-fix) | 12.8% | 38.9%                    |
| 100       | (not separately measured pre-fix) | 13.7% | 36.8%                    |

Roughly a **2x reduction** in `--no-plots` mode at 1kHz. With the full
sparkline UI the improvement is smaller in relative terms -- Sparkline
widget rendering is now the dominant remaining cost rather than the
sampler loop itself. That's tracked as a deferred option (dirty-check
guard before reassigning `.data`/calling `.update()` when a value hasn't
meaningfully changed since the last tick) in the CPU-reduction options
plan, not yet implemented.

Achieved rate was re-verified at all four target rates after this change
and still matches target closely (see table above's "achieved" figures in
the code's own diagnostic output); the full baseline-test/energy-capture/
CSV/JSONL pipeline was also re-verified end-to-end against the new
ring-buffer-based `Sampler` to confirm no behavior regression from the
storage-format change.

## Hardware notes

- **jetson-stats version pinning:** this project pins `jetson-stats==4.3.2`
  in `pyproject.toml` to match the `jtop` *system service* version already
  running on `pocket-infer-6a8f` (from L4T's default install). jtop's
  client/service protocol is version-checked and refuses to connect on a
  mismatch. If you see `Mismatch version jtop service: [X] and client: [Y]`,
  either pin the client version to match (`uv add jetson-stats==X`) or run
  `sudo jtop --install-service` to upgrade the service to match the client
  (the latter needs sudo and affects other users of the shared device --
  prefer pinning the client).
- Shunt resistor value assumed: **5 mΩ** (`shunt1_resistor` sysfs attribute
  read back as 5000 µΩ on the reference device) — matches TI reference
  design constant used by the kernel driver. If porting to a different
  Jetson board/carrier, verify this against that board's schematic before
  trusting absolute current values (the *rate* achieved is independent of
  this constant, but mA/mWh accuracy is not).
- Channel 1 (VDD_IN) = total board input power, i.e. everything downstream
  of the barrel jack / USB-C PD input — this is what "energy efficiency of
  the whole board" tests should use. Channels 2/3 (VDD_CPU_GPU_CV,
  VDD_SOC) are available in `ina3221.py` if you want to isolate a rail
  (note: only one channel can run at the ~1620 Hz single-channel rate at a
  time; enabling multiple channels drops the aggregate rate proportionally).

## Known limitations / follow-ups

- Bus voltage is currently re-read once per UI tick (~12 Hz) rather than
  every sample, since VDD_IN is a stiff supply and doesn't need 1kHz
  resolution for the mWh integration to be accurate — if your supply is
  noisy/sagging under load, consider sampling bus voltage synchronously
  with current instead (see `Sampler._run`).
- Not tested on non-RT kernels other than stock L4T 36.5.0 (`5.15.185-tegra`
  on `pocket-infer-6a8f`). Achieved rate/jitter will vary on other kernels
  or under heavy competing CPU load — the in-app status bar reports live
  jitter so you can judge data quality for a given run.
- This project is intentionally scoped to `pocket-infer-6a8f`'s Orin Nano
  Super. Porting to another Jetson requires re-verifying the I2C address,
  bus number, and shunt resistor value against that board's schematic.
