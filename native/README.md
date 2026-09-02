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

**Update (2026-09-02):** the user confirmed similar performance
characteristics on the affected device too, and decided to skip Cython
entirely in favor of a full pure-C reimplementation as a fallback CLI
option -- see below.

## `jeu` -- full pure-C CLI (baseline + energy tests)

`main.c` + supporting modules (`common.*`, `ina3221.*`, `sampler.*`,
`sysinfo.*`, `sysmon.*`, `csv_log.*`, `jsonl_log.*`, `tests.*`) implement
the **exact same workflow** as `src/app.py`'s `--headless` mode, entirely
in C, as a standalone fallback binary alongside the Python implementation
(not a replacement -- the Python TUI/`--headless` mode is unaffected).

Build:
```bash
cd native
make          # builds ./jeu
make bench    # also builds ./ina_bench (see above)
```

Usage (deliberately mirrors `app.py --headless <test>` flag-for-flag
where it makes sense):
```bash
# Baseline: waits --duration seconds (default 10) collecting idle current.
./jeu baseline --hz 1000 --duration 10 --channel VDD_IN

# Energy: starts capturing immediately; press any key to stop (falls back
# to Ctrl-C if stdin isn't an interactive terminal, same as the Python CLI).
./jeu energy --hz 1000 --channel VDD_IN

./jeu --help   # full flag list, including --sysinfo-hz
```

No sudo needed (same as the Python driver -- requires your user to be in
the `i2c` group). Writes the same CSV/sysinfo-CSV/`results.jsonl` files,
in the same `$XDG_CACHE_HOME/jetson-energy-usage` (or
`~/.cache/jetson-energy-usage`) directory, with the **same schema** as
the Python implementation, so records from both interleave cleanly in one
`results.jsonl` and CSVs are interchangeable for downstream tooling.
Prints the same human-readable summary format to stdout.

### Module map (mirrors the Python source file-for-file)

| native/           | Python equivalent      | Notes |
|--------------------|-------------------------|-------|
| `ina3221.h/.c`     | `src/ina3221.py`        | Same register map/config word/conversion math. |
| `sampler.h/.c`     | `src/sampler.py`        | Same ring buffer + `clock_nanosleep` pacing design, now native (no ctypes/GIL). |
| `sysinfo.h/.c`     | `src/sysinfo.py`        | **No jtop dependency at all** -- reads `/proc/stat` (CPU), `/sys/.../gpu/load` (GPU), `/proc/meminfo` (RAM/swap), `/sys/class/hwmon/hwmonN/rpm` + `.../pwm1` (fan, path auto-discovered), and `/sys/class/thermal/thermal_zone*` (temp, prefers the "tj" zone) directly. |
| `sysmon.h/.c`      | `sysinfo.py`'s `SysMonitor`/`SysRecorder` | Background poll thread + test-window sample recorder/summarizer. |
| `csv_log.h/.c`     | `src/csv_log.py`        | Identical column layout for both the per-sample and `_sysinfo` CSVs. |
| `jsonl_log.h/.c`   | `src/jsonl_log.py` + `tests.py`'s `_log_result_jsonl()` | Hand-rolled minimal JSON writer (no library dependency) producing the same field set. |
| `tests.h/.c`       | `src/tests.py`          | Same baseline-test and start/stop `EnergyCapture` logic, same trapezoidal mWh/mAh integration. |
| `main.c`           | `src/app.py`'s `run_headless()` | CLI arg parsing, raw/cbreak-terminal keypress handling for the energy test's stop signal, orchestration. |

### Design notes / pitfalls found while building this

- **No jtop dependency**: rather than shelling out to jtop or linking
  against its Python client (impractical from C), all system-resource
  stats are read directly from the same underlying `/proc` and `/sys`
  files jtop itself parses (confirmed by reading jtop's own source for
  each field -- e.g. GPU load is `raw_value / 10.0` from
  `.../17000000.gpu/load`, matching `jtop/core/gpu.py`). This has a nice
  side effect: it's immune by construction to the CPU-percent
  square-wave glitch documented in `sysinfo.py`'s module docstring, since
  that bug lives in the jtop *service* itself, which this code never
  talks to.
- **Fan sysfs paths are discovered at runtime**, not hardcoded, since
  `hwmonN` numbering isn't guaranteed stable across kernel versions/board
  revisions -- `sysinfo_find_fan_paths()` scans `/sys/class/hwmon/hwmon*`
  for the one exposing an `rpm` file.
- **`ioctl(fd, I2C_SLAVE, ...)` must NOT be called** -- same pitfall as
  `ina_bench.c`: the kernel hwmon driver already owns the INA3221's I2C
  address, so claiming exclusive ownership via `I2C_SLAVE` fails with
  `EBUSY`. `I2C_RDWR` messages carry the target address per-transaction
  instead, coexisting fine with the kernel driver.
- **A literal `*/` inside a C block comment terminates it early** --
  hit this once describing a glob-like sysfs path
  (`/sys/.../thermal_zone*/type`) in a header docstring; rewritten to
  avoid the sequence.
- **JSON output uses a small hand-rolled writer**, not a library: the
  schema is fixed and shallow (no nested objects beyond what's already
  flattened, e.g. `sys_avg_cpu_percent` instead of a nested `sysinfo: {}`
  object) so a dependency wasn't worth adding for this.
- **Terminal raw/cbreak mode (`termios`) mirrors `app.py`'s
  `_raw_terminal()`** exactly: only engaged if stdin `isatty()`, restored
  via the original `termios` state on exit either way (normal exit or
  Ctrl-C), matching the Python fallback-to-Ctrl-C behavior when stdin
  isn't a real terminal (piped/redirected input).

### Verification performed

- `./jeu --help`, `baseline --duration <N>`, and `energy` (via a real PTY
  keypress, Ctrl-C on a PTY, and Ctrl-C/SIGINT with non-tty piped stdin)
  all tested on `pocket-infer-6a8f`.
- Confirmed output CSV (`sample_index,elapsed_s,current_ma[,power_mw]`)
  and sysinfo CSV columns match `csv_log.py`'s format exactly.
- Confirmed `results.jsonl` entries are valid JSON (`python3 -m
  json.tool`) with the same field set/naming as the Python
  implementation's records, so both can coexist in the same file.
- CPU footprint (via `wait4()`/`getrusage()`, same methodology as the
  `ina_bench` benchmark above): `./jeu baseline --duration 10 --hz 1000`
  measured **~0.585-0.591s total CPU time / 10s run (~5.8-5.9%)** --
  matching `ina_bench`'s standalone reference point (~5.4%) almost
  exactly, and about **3.3x cheaper** than the Python `--headless`
  baseline (~19.5%) at the same target rate. This is exactly the outcome
  hoped for: `jeu` inherits the full CPU-footprint benefit the `ina_bench`
  spike identified, while also implementing the complete test workflow
  (not just raw sampling), giving the user a genuine low-overhead
  fallback option alongside the Python app.

## TUI: raw ANSI + hand-rolled diffing, no ncurses/Cython (2026-09-02)

Once `jeu`'s headless baseline/energy tests were validated, the user
asked to bring back an interactive TUI with live sparklines (current/CPU/
GPU/temp) -- the one thing the pure-CLI rewrite dropped -- while staying
"as lean as possible" on CPU. Before implementing, three options were
weighed:

1. **Cython** -- wrap the C sampler and drive a Python-side render loop.
   Rejected: this puts the *render loop* back under the GIL, which is
   exactly what caused the original TUI's overhead (jitter from a busy
   UI thread was the very first bug fixed in this project's history --
   see `sampler.py`'s `sys.setswitchinterval` docstring). Also means
   maintaining two toolchains/an FFI boundary for no benefit, since the
   bottleneck this whole effort is chasing lives in the render loop, not
   the sampler.
2. **ncurses** -- terminfo-portable, handles resize/redraw diffing
   automatically. Rejected: its main value (terminal-portability,
   generic damage-tracking for an unknown/dynamic widget tree) doesn't
   apply here -- the layout is fixed and known ahead of time (2 status
   rows + 4 sparkline rows + a short log + a footer), so a hand-rolled
   diff is just as effective and avoids a new library dependency
   (`libncursesw` + terminfo database) for a benefit we don't need.
3. **Raw ANSI + a small hand-rolled diffing renderer** -- chosen. Same
   core idea ncurses/Textual use internally (only touch terminal cells
   that actually changed since the last frame), implemented directly for
   our fixed layout in a few hundred lines of C, no new dependencies.

### New modules

| File              | Purpose |
|--------------------|---------|
| `term.h/.c`        | Raw/cbreak terminal mode, alternate screen buffer, cursor show/hide, `ioctl(TIOCGWINSZ)` terminal-size query. |
| `sparkline.h/.c`   | 8-level Unicode block-glyph sparklines (`▁▂▃▄▅▆▇█`, same codepoint range Textual's `Sparkline` and tools like `holman/spark` use) with per-render auto min/max scaling, matching the old TUI's visual fidelity. |
| `screen.h/.c`      | Fixed-row double-buffered renderer: build a frame as an array of text lines, `screen_flush()` only emits ANSI cursor-position + clear-to-eol + text for lines that changed (byte-compare against the previous frame) -- the "damage diffing" ncurses/Textual do internally, hand-rolled for our known layout. Also truncates each line to the current terminal width in *display columns* (UTF-8-aware, so a 3-byte sparkline glyph counts as 1 column, not 3) so nothing wraps onto the next physical row and breaks the fixed-row assumption. |
| `tui.h/.c`         | Application state + render loop + keybindings (`b`/`e`/space/`r`/`q`, same as the old Python TUI), 5Hz tick (matches the old `UI_REFRESH_HZ`), baseline test runs in its own pthread so the render loop never blocks on it (mirrors the old Textual `work`-decorated worker thread). |

`main.c`'s dispatch now defaults to `tui` mode when no test name is
given (`./jeu` with no args = TUI, same as before this change `./jeu`
alone was an error) -- `baseline`/`energy` still work exactly as before.

### Pitfall found + fixed

Early testing showed status-line text wrapping onto the sparkline row
below it, corrupting the display, because the Mode/Channel/Latest and
CPU/GPU/RAM/Swap/Fan lines were wider than the test terminal (100
columns) and the terminal itself wrapped the overflow onto the next
physical row -- breaking the "one logical line = one physical row"
assumption `screen.c`'s diffing renderer depends on. Fixed by having
`screen_set_line()` truncate every line to the terminal's current width
(via a new `screen_set_term_width()` call once per tick, UTF-8-aware so
sparkline glyphs aren't split mid-byte).

### Verification performed

- Captured raw PTY output from a real `jeu tui` run and replayed it
  through Python's `pyte` terminal emulator (temporarily added as a dev
  dependency, then removed) to get a readable rendered-screen dump --
  confirmed the status/sysinfo lines, all four sparklines, the log tail,
  and the footer all render on their own distinct rows with no wrapping.
- Scripted a full interaction sequence via PTY: start the TUI, press
  `b` to run a 10s baseline test, confirm the "BASELINE RUNNING..." mode
  text and sparklines update live during the test, confirm the completed
  test's summary lines appear in the log tail afterward, press `r` to
  reset sparkline history (confirmed a flat/near-empty sparkline
  immediately after), then `q` to quit -- confirmed clean exit (code 0),
  cursor restored, alternate screen buffer torn down (`\x1b[?25h`
  `\x1b[?1049l` observed in the captured output).
- CPU footprint (`wait4()`/`getrusage()`, 1000Hz, 10s PTY run): **`jeu
  tui` measured ~0.617s total CPU time / 10s (~6.2%)** -- essentially
  matching the pure `jeu baseline`/`ina_bench` floor (~5.4-5.9%), meaning
  this renderer adds almost no CPU cost on top of sampling. For
  reference, the old Python Textual TUI measured **~4.116s / 10s
  (~41.2%)** under the same methodology -- the native TUI is roughly
  **6.6x cheaper**.
- Fixed a `--help`/`-h` argument-parsing regression introduced while
  adding the `tui` test-name dispatch (the positional-arg detection was
  short-circuiting before the `--help` check ran) -- caught by manual
  testing, not by any automated test suite (none exists for this native
  code yet).
