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
- **`src/jsonl_log.py`** — every test run also appends one JSON object (one
  line) to a single running log, `results.jsonl`, in the same cache
  directory. One `b` invocation writes exactly one line; one complete `e`
  start/stop capture writes exactly one line. Different test types can
  carry different fields (e.g. only the energy test has `mwh`/`mah`; only
  the baseline test has `requested_duration_s`) -- consumers should key off
  `test_type` rather than assuming a fixed schema. Each record also carries
  sampler jitter/rate stats and device config (channel, shunt resistance,
  I2C address) so runs can be compared/audited later. Built via
  `tests._log_result_jsonl()`, called from both `run_baseline_test` and
  `EnergyCapture.stop`.
- **`src/app.py`** — the Textual TUI: live current chart (last 15s), status
  bar (latest reading, achieved sample rate, jitter), scrolling test-result
  log, and keybindings.

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

No `sudo` needed as long as your user is in the `i2c` group:

```bash
groups   # should list "i2c"
```

## Hardware notes

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
