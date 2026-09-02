# Jetson Energy Usage (`jeu`)

A lightweight terminal tool for running energy-efficiency tests on NVIDIA
Jetson boards, using the onboard **INA3221** current/voltage monitor (I2C
address `0x40`, bus 1) — no external hardware, no root required. Written in
pure C with no runtime dependencies beyond the C standard library, `libm`,
and `libpthread`.

It has two faces:

- **`jeu tui`** — an interactive terminal UI with live current/CPU/GPU/
  die-temp sparklines, meant for watching a test run in real time.
- **`jeu baseline`** / **`jeu energy`** — a minimal, no-TUI CLI mode meant
  to run *alongside* your own application with the smallest possible CPU
  footprint (see [Performance](#performance) below).

Both modes write the same CSV / JSON-lines log files, so you can start in
`tui` mode to sanity-check your setup, then switch to headless `baseline`/
`energy` runs for actual measurements without disturbing the thing you're
trying to measure.

## End-to-end demo:


https://github.com/user-attachments/assets/efc348e9-568a-499c-a89e-c5fa5baaa9a7


## Why direct register access instead of the kernel `hwmon` driver?

The stock L4T kernel driver exposes the INA3221 via
`/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon1/`, but on the reference
hardware it's configured to round-robin all 3 channels with a slow
bus-voltage conversion time (~4.156 ms), giving an aggregate update rate of
only ~77 Hz — and the config isn't writable without root (`samples` /
conversion-time attributes are root-owned).

Instead, `jeu` talks to the chip directly over `/dev/i2c-1`, reconfiguring
it for single-channel (VDD_IN / total board input power by default),
minimum conversion time (140 µs), no on-chip averaging. This needs no
`sudo` as long as your user is in the `i2c` group (see [Setup](#setup)).
Measured sustained single-channel rate on the reference hardware: **~1620
Hz** (617 µs/read) — comfortably clears the 1 kHz default target with
headroom for jitter.

The original chip configuration is snapshotted on startup and restored on
exit (best-effort), so other tools reading the standard hwmon path (e.g.
`jtop`/`tegrastats`) return to their prior behavior after `jeu` exits.

## Setup

Requires `gcc`, `make`, and your user in the `i2c` group (no `sudo` needed
for actually running the tool):

```bash
groups   # should list "i2c" -- if not, `sudo usermod -aG i2c $USER` and re-login
git clone <this-repo> jeu && cd jeu
make
./jeu tui
```

To install system-wide (installs to `/usr/local/bin` by default):

```bash
sudo make install PREFIX=/usr/local
# uninstall:
sudo make uninstall PREFIX=/usr/local
```

No package manager dependencies — just a C compiler and `make`.

## Usage

```
usage: ./jeu [tui|baseline|energy] [--hz N] [--duration SEC] [--channel NAME]
             [--color|--no-color]

  tui                Interactive terminal UI with live sparklines (current/
                     CPU/GPU/temp) -- keybindings b/e/space/r/q (see below).
                     This is the default when no test name is given.
  baseline           Waits (idle) --duration seconds (default 10) collecting
                     current samples, then prints/logs a results summary.
  energy             Starts capturing immediately; press any key to stop
                     (falls back to Ctrl-C if stdin isn't an interactive
                     terminal), then prints/logs a results summary.
  --hz N             Target INA3221 current sampling rate in Hz (default 1000).
  --duration SEC     Duration in seconds for 'baseline' (default 10). Ignored
                     for 'energy'/'tui'.
  --channel NAME     INA3221 rail to sample: VDD_IN, VDD_CPU_GPU, or VDD_SOC
                     (default VDD_IN).
  --sysinfo-hz N     Poll rate in Hz for CPU/GPU/RAM/swap/fan/temp (default 2).
  --comment TEXT     Attach a one-off note to this run's result (console
                     summary and JSONL log). Applies only to this single
                     'baseline'/'energy' invocation. Ignored for 'tui' (use
                     the 'e' keybinding's prompt there instead).
  --color            Force-enable truecolor sparkline gradients in 'tui' mode.
                     On by default when stdout is a terminal and NO_COLOR/
                     TERM=dumb aren't set.
  --no-color         Disable sparkline colors -- plain glyphs only. Use this
                     if your terminal/multiplexer doesn't render truecolor
                     (24-bit RGB) escape codes well.
```

### `tui` mode — keybindings

| Key     | Action |
|---------|--------|
| `b`     | Run a **baseline (idle) power test** (10s, averages current). |
| `e`     | Prompt for an optional one-line **comment**, then **arm** the energy-usage test (`space` works either way once armed). Type your note and press Enter to confirm, or Esc to skip. The comment is attached to that one energy-test result only — it's cleared automatically afterward, so the next capture starts with no comment unless you press `e` again. |
| `space` | **Start/stop** an energy capture. On stop, integrates current × time (trapezoidal rule) over the window to report mWh / mAh consumed, plus mean/min/max current and achieved sample rate. |
| `r`     | Clear the live sparkline history. |
| `q`     | Quit (restores original INA3221 config). |

### `baseline` / `energy` — headless CLI mode

For running alongside your own application with the smallest possible
overhead — no TUI, no live sensor output on screen at all:

```bash
# Baseline: waits 10s (or --duration N) collecting idle current, then reports.
./jeu baseline --hz 1000 --duration 10 --channel VDD_IN

# Energy: starts capturing immediately; press any key to stop and report
# (falls back to Ctrl-C if stdin isn't an interactive terminal, e.g. run
# from a script with redirected/piped stdin).
./jeu energy --hz 1000 --channel VDD_IN

# Attach a note to a specific run -- shows up in the console summary and
# as a "comment" field in results.jsonl. One-off, applies to this run only.
./jeu baseline --duration 30 --comment "after fan curve change"
```

Both modes print only a one-line "started, collecting..." status message
before the test, and a human-readable summary after (mean current, min/max,
bus voltage, mean power, energy for the energy test, CPU/GPU/temp avg/max,
and the CSV/JSONL file paths). No sparklines, no periodic sensor printouts.

## Output files

Every test run writes to `$XDG_CACHE_HOME/jetson-energy-usage` (falling
back to `~/.cache/jetson-energy-usage`):

- **`YYYYMMDD_HHMMSS_<baseline|energy>.csv`** — raw per-sample data
  (elapsed time, instantaneous current, estimated instantaneous power).
- **`YYYYMMDD_HHMMSS_<baseline|energy>_sysinfo.csv`** — one row per
  system-resource snapshot taken during the same test window (CPU%, GPU%,
  RAM/swap used+total+%, fan RPM/duty, die temp).
- **`results.jsonl`** — one JSON object per line, appended across every
  test run ever performed (one `baseline` run or one complete `energy`
  start/stop cycle = one line). Different test types carry different
  fields (e.g. only `energy` has `mwh`/`mah`) — key off `test_type` rather
  than assuming a fixed schema. Each record also carries sampler
  jitter/rate stats, device config (channel, shunt resistance, I2C
  address), system-resource summary stats over the test window, and an
  optional `comment` field (`null` if none was given for that run — see
  `--comment` above / the TUI's `e` keybinding).

## Performance

`jeu` was rewritten from an earlier Python/Textual prototype specifically
to minimize CPU footprint, since the intended use case is running
*alongside* the user's own application on a resource-constrained device.
Measured via `wait4()`/`getrusage()` (total process CPU time = `ru_utime +
ru_stime`), 1000 Hz target, 10s runs, otherwise-idle device:

| mode                          | total CPU time / 10s run | ~% of one core |
|--------------------------------|---------------------------|-----------------|
| `jeu baseline`                 | ~0.585-0.591 s             | **~5.8%**       |
| `jeu tui` (full sparkline UI)   | ~0.617 s                   | **~6.2%**       |

For context, the earlier Python prototype measured ~19.5% (headless) and
~41.2% (full TUI) under the same methodology — see
[`docs/DEVLOG.md`](docs/DEVLOG.md) for the full investigation, including
why Cython/ncurses were considered and rejected in favor of a from-scratch
C rewrite.

## Hardware notes

- **Shunt resistor**: assumed **5 mΩ**, matching the TI reference design
  constant used by the kernel driver on the reference hardware. If
  porting to a different Jetson board/carrier, verify this against that
  board's schematic before trusting absolute current values (the achieved
  sample *rate* is independent of this constant, but mA/mWh accuracy is
  not) — see `ina3221.h`'s `SHUNT_OHMS` constant in `src/main.c`.
- **Channel 1 (VDD_IN)** = total board input power, i.e. everything
  downstream of the barrel jack / USB-C PD input — the default, and what
  "energy efficiency of the whole board" tests should use. Channels 2/3
  (`VDD_CPU_GPU`, `VDD_SOC`) are available via `--channel` if you want to
  isolate a rail (note: only one channel runs at the ~1620 Hz
  single-channel rate at a time; enabling multiple channels drops the
  aggregate rate proportionally — not currently supported by this tool).
- Developed and tested primarily on a Jetson Orin Nano Super running stock
  L4T 36.5.0 (`5.15.185-tegra`). Achieved rate/jitter will vary on other
  kernels or under heavy competing CPU load — `jeu tui`'s status bar
  reports live jitter so you can judge data quality for a given run.

## Architecture

All source lives in `src/`; `make` builds it into `build/` (gitignored)
and links the final `jeu` binary at the repo root.

| File(s)            | Purpose |
|---------------------|---------|
| `src/ina3221.h/.c`       | Raw INA3221 register driver (config word encode/decode, shunt/bus voltage register reads, single-channel fast-mode helper). |
| `src/sampler.h/.c`       | Background thread sampling current at a target rate into a preallocated ring buffer, using `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` for absolute-deadline pacing to minimize jitter. |
| `src/sysinfo.h/.c`       | Reads CPU/GPU/RAM/swap/fan/temp directly from `/proc` and `/sys` — no `jtop`/`jetson-stats` dependency. |
| `src/sysmon.h/.c`        | Background poll thread wrapping `sysinfo.c`, plus a test-window sample recorder/summarizer. |
| `src/csv_log.h/.c`       | Per-sample and per-test-window sysinfo CSV writers (buffered, batched I/O). |
| `src/jsonl_log.h/.c`     | Hand-rolled minimal JSON writer appending to `results.jsonl`. |
| `src/tests.h/.c`         | Baseline-test and start/stop `EnergyCapture` logic, including trapezoidal mWh/mAh integration. |
| `src/term.h/.c`          | Raw/cbreak terminal mode, alternate screen buffer, cursor show/hide, terminal-size query. |
| `src/sparkline.h/.c`     | Unicode block-glyph sparklines (8-level, optionally stacked across rows for more vertical resolution) with optional truecolor gradients. |
| `src/screen.h/.c`        | Fixed-row, double-buffered terminal renderer — only repaints lines that actually changed since the last frame. |
| `src/tui.h/.c`           | Interactive TUI application state, render loop, and keybindings. |
| `src/main.c`             | CLI argument parsing and mode dispatch (`tui`/`baseline`/`energy`). |
| `src/ina_bench.c`        | Standalone I2C-sampling benchmark used as a performance reference point during development — see `docs/DEVLOG.md`. Not part of the `jeu` binary; build with `make bench`. |

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for build/test conventions and
[`docs/DEVLOG.md`](docs/DEVLOG.md) for the design history and rationale
behind non-obvious choices (why C instead of Python, why raw ANSI instead
of ncurses, etc.) — worth reading before making structural changes.

## License

[MIT](LICENSE).
