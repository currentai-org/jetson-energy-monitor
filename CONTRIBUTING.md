# Contributing

## Building

```bash
make          # builds ./jeu
make bench    # also builds ./ina_bench, the standalone sampling benchmark
make clean    # removes build artifacts
```

No external dependencies beyond `gcc`/`clang`, `make`, and the C standard
library + `libm` + `libpthread` (all present on any standard Linux
install). `CC` and `CFLAGS` are overridable, e.g. `make CC=clang CFLAGS='-O2
-Wall -Wextra -std=gnu11 -fsanitize=address'` for a debug build.

## Code style

- **C11**, no compiler-specific extensions beyond what glibc/gnu11
  provides (`clock_nanosleep`, `pthread_*`).
- Every non-trivial function gets a comment explaining *why*, not just
  *what* — this codebase has accumulated several non-obvious fixes (GIL
  interactions in the old Python version, a jtop service bug, ANSI escape
  sequence edge cases) that are easy to accidentally regress if the
  reasoning isn't written down. See `docs/DEVLOG.md` for the full history.
- Prefer explicit, bounded buffers and `snprintf`/`strn*` over anything
  that can silently overflow. This tool talks directly to hardware and
  runs unattended for long stretches — a crash or corrupted read is worse
  than a slightly verbose bounds check.
- Keep the CPU footprint front-of-mind for anything in the sampler
  (`sampler.c`) or render (`tui.c`/`screen.c`/`sparkline.c`) hot paths —
  this tool's entire reason for existing in C rather than Python is
  minimizing overhead so it can run alongside the user's real workload.
  If you're adding something to those paths, measure before/after with
  the methodology in `docs/DEVLOG.md`'s Performance section
  (`wait4()`/`getrusage()`-based, not `ps`'s coarse instantaneous `%CPU`).

## Testing

There is no automated test suite yet (see `docs/DEVLOG.md`'s notes on this
— manual PTY-based testing has caught real bugs during development, but
isn't automated). If you're adding a test harness, a reasonable starting
point:

- Unit tests for the pure-computation modules (`sparkline.c`'s rendering
  math, `tests.c`'s trapezoidal integration, `csv_log.c`/`jsonl_log.c`'s
  output formatting) don't need real hardware and are the easiest wins.
- Integration testing against real I2C hardware isn't mockable without
  significant effort; manual verification against `pocket-infer-6a8f` (or
  your own Jetson) remains the practical path for `ina3221.c`/`sampler.c`.
- For TUI changes, capturing raw PTY output and replaying it through a
  terminal emulation library (Python's `pyte` was used ad hoc during
  development, not currently a repo dependency) is an effective way to
  verify rendering without a human watching a live terminal.

## Before submitting a change

1. `make clean && make` — confirm a clean build with no new warnings.
2. If your change touches the sampler or renderer hot paths, include a
   before/after CPU measurement in your PR description (see Testing
   above for methodology).
3. If you fix a non-obvious bug, add a note to `docs/DEVLOG.md` explaining
   the root cause — future contributors (including future you) will
   thank you.

## Porting to a different Jetson board

This project was developed and tuned against one specific board (Jetson
Orin Nano Super, see `README.md`'s Hardware notes). Porting to a different
Jetson requires re-verifying, at minimum:

- The I2C bus number and INA3221 address (`I2C_BUS_NUM`/`I2C_ADDRESS` in
  `main.c`).
- The shunt resistor value (`SHUNT_OHMS` in `main.c`) — check your board's
  schematic; getting this wrong doesn't break the *rate* achieved but does
  break mA/mWh accuracy.
- The GPU-load sysfs path in `sysinfo.c` (`GPU_LOAD_PATH`) — this is
  SoC-specific and may differ between Jetson generations.
- Fan/thermal sysfs paths in `sysinfo.c` are discovered at runtime (not
  hardcoded), so they should be more portable, but haven't been verified
  on hardware other than the reference device.
