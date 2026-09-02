# Development log

This is a running log of significant design decisions, performance
investigations, and bugs found/fixed during this project's development. It's
kept for historical context and to explain *why* certain non-obvious choices
were made -- if you're just looking to build and run the tool, see the main
[README.md](../README.md) instead.

## Background: why C instead of Python

This project started as a Python/Textual TUI. It was fully rewritten in pure
C (no Cython, no ncurses) once profiling showed CPU/GIL overhead in the
Python implementation was large enough to matter for a tool meant to run
*alongside* the user's own application on a shared, resource-constrained
device. The sections below trace that investigation chronologically.

## Pure-C sampling benchmark (`ina_bench.c`) as a reference point

Before committing to a full rewrite, a minimal **standalone benchmark** was
built to establish a reference point: just read the INA3221 over raw I2C
ioctl, queue samples in a preallocated ring buffer, and periodically flush a
batch to a CSV file with a single buffered `fwrite()` rather than one
`fprintf()` per sample. Uses `clock_nanosleep(CLOCK_MONOTONIC,
TIMER_ABSTIME, ...)` for absolute-deadline pacing (no busy-wait imprecision,
no GIL).

```bash
gcc -O2 -Wall -o ina_bench ina_bench.c -lm
./ina_bench --hz 1000 --duration 10 --channel 1 --out /tmp/ina_bench.csv
```

**Benchmark results (pocket-infer-6a8f, 2026-09-02):**

Measured via a small `wait4()`/`getrusage()`-based wrapper (total process
CPU time = `ru_utime + ru_stime`, summed across the whole run -- more
precise than reading instantaneous `%CPU` off `ps`, which is too coarse at
these levels), 1000 Hz target, channel 1 (VDD_IN), 10s runs, clean
otherwise-idle device (`uptime` load average <0.2 throughout):

| implementation                              | total CPU time / 10s run | ~% of one core |
|----------------------------------------------|---------------------------|-----------------|
| `ina_bench` (pure C)                          | 0.54 - 0.55 s              | **~5.4%**       |
| Python `--headless` baseline (no Textual)     | 1.93 - 1.97 s              | **~19.5%**      |

Both hit the 1000 Hz target rate essentially exactly (achieved rate
998.5-1000.0 Hz in both cases -- achieved rate itself wasn't the problem, CPU
footprint was). The pure-C reference point was **roughly 3.5x cheaper** than
the best-case Python path at the same target rate, confirming real headroom
below the Python floor (plausibly per-sample I2C wrapper overhead, GIL
bookkeeping, small-object allocation -- not an unavoidable kernel/hardware
floor). This made the case for a Cython/C-extension approach as the next
step -- which then evolved into a full pure-C rewrite once the user decided
to skip Cython in favor of a complete native fallback CLI.

## Full pure-C CLI (`jeu`)

`main.c` + supporting modules (`common.*`, `ina3221.*`, `sampler.*`,
`sysinfo.*`, `sysmon.*`, `csv_log.*`, `jsonl_log.*`, `tests.*`) reimplemented
the Python `--headless` mode's exact workflow entirely in C, as a standalone
fallback binary alongside the Python implementation (at the time, not yet a
full replacement).

**Design notes / pitfalls found while building this:**

- **No jtop dependency**: rather than shelling out to jtop or linking
  against its Python client (impractical from C), all system-resource
  stats are read directly from the same underlying `/proc` and `/sys`
  files jtop itself parses (confirmed by reading jtop's own source for
  each field -- e.g. GPU load is `raw_value / 10.0` from
  `.../17000000.gpu/load`, matching `jtop/core/gpu.py`). This has a nice
  side effect: it's immune by construction to a CPU-percent square-wave
  glitch discovered in the jtop *service* itself (see below).
- **Fan sysfs paths are discovered at runtime**, not hardcoded, since
  `hwmonN` numbering isn't guaranteed stable across kernel versions/board
  revisions -- `sysinfo_find_fan_paths()` scans `/sys/class/hwmon/hwmon*`
  for the one exposing an `rpm` file.
- **`ioctl(fd, I2C_SLAVE, ...)` must NOT be called** -- the kernel hwmon
  driver already owns the INA3221's I2C address, so claiming exclusive
  ownership via `I2C_SLAVE` fails with `EBUSY`. `I2C_RDWR` messages carry
  the target address per-transaction instead, coexisting fine with the
  kernel driver.
- **A literal `*/` inside a C block comment terminates it early** --
  hit this once describing a glob-like sysfs path
  (`/sys/.../thermal_zone*/type`) in a header docstring; rewritten to
  avoid the sequence.
- **JSON output uses a small hand-rolled writer**, not a library: the
  schema is fixed and shallow, so a dependency wasn't worth adding.
- **Terminal raw/cbreak mode (`termios`)** is only engaged if stdin
  `isatty()`, restored via the original `termios` state on exit either
  way (normal exit or Ctrl-C).

**Measured CPU footprint** (`wait4()`/`getrusage()`, same methodology as
`ina_bench`): `jeu baseline --duration 10 --hz 1000` measured
**~0.585-0.591s total CPU time / 10s run (~5.8-5.9%)** -- matching
`ina_bench`'s standalone reference point almost exactly, and about **3.3x
cheaper** than the old Python `--headless` baseline (~19.5%) at the same
target rate, while implementing the complete test workflow, not just raw
sampling.

## Interactive TUI: raw ANSI + hand-rolled diffing, no ncurses/Cython

Once `jeu`'s headless baseline/energy tests were validated, an interactive
TUI with live sparklines (current/CPU/GPU/temp) was brought back -- the one
thing the pure-CLI rewrite had dropped -- while staying as lean as possible
on CPU. Three implementation options were weighed:

1. **Cython** -- wrap the C sampler and drive a Python-side render loop.
   Rejected: this puts the *render loop* back under the GIL, which is
   exactly what caused the original TUI's overhead in the first place.
   Also means maintaining two toolchains/an FFI boundary for no benefit.
2. **ncurses** -- terminfo-portable, handles resize/redraw diffing
   automatically. Rejected: its main value (terminal-portability, generic
   damage-tracking for an unknown/dynamic widget tree) doesn't apply here
   -- the layout is fixed and known ahead of time (2 status rows + 4
   sparkline rows + a short log + a footer), so a hand-rolled diff is just
   as effective and avoids a new library dependency for a benefit not
   needed here.
3. **Raw ANSI + a small hand-rolled diffing renderer** -- chosen. Same
   core idea ncurses/Textual use internally (only touch terminal cells
   that actually changed since the last frame), implemented directly for
   the fixed layout in a few hundred lines of C, no new dependencies.

**New modules:**

| File              | Purpose |
|--------------------|---------|
| `term.h/.c`        | Raw/cbreak terminal mode, alternate screen buffer, cursor show/hide, `ioctl(TIOCGWINSZ)` terminal-size query. |
| `sparkline.h/.c`   | 8-level Unicode block-glyph sparklines (`▁▂▃▄▅▆▇█`) with per-render auto min/max scaling. |
| `screen.h/.c`      | Fixed-row double-buffered renderer: `screen_flush()` only emits ANSI cursor-position + clear-to-eol + text for lines that changed (byte-compare against the previous frame). Also truncates each line to the current terminal width in *display columns* (UTF-8- and ANSI-escape-aware). |
| `tui.h/.c`         | Application state + render loop + keybindings (`b`/`e`/space/`r`/`q`), 5Hz tick, baseline test runs in its own pthread so the render loop never blocks on it. |

**Pitfall found + fixed:** status-line text wider than the terminal wrapped
onto the sparkline row below it, corrupting the display, because the
terminal itself wraps overflow onto the next physical row -- breaking the
"one logical line = one physical row" assumption the diffing renderer
depends on. Fixed by having `screen_set_line()` truncate every line to the
terminal's current width (UTF-8-aware so glyphs aren't split mid-byte).

**Measured CPU footprint** (`wait4()`/`getrusage()`, 1000Hz, 10s PTY run):
`jeu tui` measured **~0.617s total CPU time / 10s (~6.2%)** -- essentially
matching the pure `jeu baseline`/`ina_bench` floor, meaning the renderer
adds almost no CPU cost on top of sampling. For reference, the old Python
Textual TUI measured **~4.116s / 10s (~41.2%)** under the same methodology
-- the native TUI came out roughly **6.6x cheaper**.

## Log region: fixed height -> dynamic terminal-height scaling

The scrolling test-result log region was originally a fixed number of
visible lines; later made to scale with the terminal's actual row count
(`compute_log_visible_lines()`), reserving a fixed "chrome" row budget
(status/sysinfo/sparkline/footer rows) and giving everything else to the
log, clamped to a sensible min/max so very short or very tall terminals
both render sensibly.

## Vertical resolution: 2-row stacked sparklines + color gradients

The 8 block-level glyphs (`▁`-`█`) give only 8 distinguishable heights per
character cell. Four approaches to increasing effective vertical resolution
were considered:

1. **Stack N rows per metric (chosen, N=2)** -- `sparkline_render_rows()`
   distributes each value's fill level bottom-up across N stacked terminal
   rows, giving `N*8` distinguishable levels (16 at N=2) using the exact
   same 8 glyphs, same underlying sample data, at **zero extra CPU cost**
   (confirmed via `getrusage()`: ~6.1% with 2-row sparklines vs ~6.2% with
   1-row, within noise).
2. **Braille dot-matrix characters (U+2800-U+28FF)** -- each cell is a 2x4
   dot grid, giving higher resolution than stacked block rows in both
   dimensions, used by libraries like `brailliant`/`isene/plot`. Not
   implemented: correctly rendering a *line* (not just point samples) in
   dot-matrix form requires tracking dot state per sub-column pair and
   connecting them -- more code and CPU per render than the current
   approach. Worth a follow-up if 16 levels isn't enough detail.
3. **ANSI truecolor foreground gradient on top of existing glyphs (chosen)**
   -- color the glyph based on value (24-bit gradient from green to a
   per-metric color: current->yellow, cpu->cyan, gpu->magenta,
   temp->red) using `\x1b[38;2;R;G;Bm`. Adds a second, continuous
   "channel" of information on top of the block-height levels.
   Implemented behind `--color`/`--no-color`, on by default (auto-detects
   tty/`NO_COLOR`/`TERM=dumb`). Confirmed effectively free via
   `getrusage()` by only emitting a new color escape when the value's
   color bucket actually changes between adjacent glyphs in the same row.
4. **Increasing history time-resolution or glyph width** -- these affect
   *horizontal* resolution, not vertical value resolution, so ruled out as
   answers to this specific question.

`SPARKLINE_ROWS` in `tui.c` is a single constant -- bumping it is a one-line
change if more vertical detail is wanted later.

**Bugs found and fixed along the way:**

- **Top-row sparkline glyphs shifted 1 column left for CPU/GPU/temp
  (not Current).** Root cause: the continuation-row padding used a single
  hardcoded prefix-width constant for all four metrics, but the CPU/GPU/
  temp format strings use a literal `"%%"` for the percent/degree sign,
  which printf collapses to one `%` character at render time -- making
  those three metrics' actual printed prefixes one character shorter than
  the hardcoded constant assumed. Fixed by computing each metric's actual
  prefix length via `snprintf` immediately before rendering that metric's
  rows, rather than sharing one constant across metrics with different
  format strings.
- **Flat-at-zero metrics rendered a solid gray/colored block instead of
  appearing genuinely idle.** The "flat series" special case (triggered
  whenever min==max across the visible window) always rendered at a fixed
  mid-level regardless of whether the flat value was zero or something
  else. Fixed: render fully blank (no glyph, no color escape) when the
  flat value itself is exactly zero; a flat *nonzero* series (e.g. pinned
  at 50%) still renders at mid-level so it remains visually distinct from
  "no data".

## CPU-load square-wave glitch (historical, Python-era jtop bug)

Before the full C rewrite, the Python implementation's `CPU:` sparkline/
readout reliably alternated between a fixed value and the real
instantaneous reading, producing a clean square-wave pattern. Root cause:
the `jtop` **system service** itself (root-owned, shared across every user
of the device) periodically resets its internal `/proc/stat` delta tracker
whenever its control queue goes briefly idle between client requests. The
very next CPU reading after a reset computes its delta against an all-zero
baseline instead of the real previous sample, yielding the *cumulative
average utilization since boot* -- confirmed to appear on every other
update (a strict 50% duty cycle), explaining the clean square wave.

This is exactly why the C rewrite's `sysinfo.c` reads `/proc/stat` directly
with its own private per-process delta-tracking state, rather than going
through jtop at all -- it's immune to this class of bug by construction.

## Achieved-rate lag (historical, Python-era GIL bug)

Also from the Python era: the achieved sampling rate used to consistently
lag the requested target rate by 10-20%. Root cause: CPython's default GIL
switch interval (5ms) is coarser than the 1ms period needed for 1kHz
sampling -- when the UI thread was CPU-busy, the interpreter only
reconsidered handing the GIL to the waiting sampler thread roughly every
5ms. Not applicable to the C rewrite (no GIL), but noted here since it's
part of why the eventual switch to C fixed accuracy as a side effect of
fixing CPU footprint.

## Hardware notes carried over from the original design

- Shunt resistor value assumed: **5 mΩ** (matches the TI reference design
  constant used by the kernel driver, confirmed by reading back the
  `shunt1_resistor` sysfs attribute as 5000 µΩ on the reference device).
  If porting to a different Jetson board/carrier, verify this against
  that board's schematic before trusting absolute current values.
- Channel 1 (VDD_IN) = total board input power, i.e. everything downstream
  of the barrel jack / USB-C PD input -- this is what "energy efficiency
  of the whole board" tests should use. Channels 2/3 (VDD_CPU_GPU_CV,
  VDD_SOC) are available if you want to isolate a rail (note: only one
  channel can run at the ~1620 Hz single-channel rate at a time; enabling
  multiple channels drops the aggregate rate proportionally).
- This project was developed and tested primarily on a Jetson Orin Nano
  Super running stock L4T 36.5.0 (`5.15.185-tegra`). Achieved rate/jitter
  will vary on other kernels or under heavy competing CPU load -- `jeu
  tui`'s status bar reports live jitter so you can judge data quality for
  a given run.
