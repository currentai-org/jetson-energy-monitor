/*
 * tui.h -- interactive terminal UI: sparklines (current/CPU/GPU/temp),
 * status line, sysinfo line, and a small scrolling log -- at a fraction
 * of the CPU cost of the old Textual-based TUI. See README.md's TUI
 * section for the design rationale (raw ANSI + hand-rolled diffing
 * instead of ncurses/Cython).
 *
 * Keybindings:
 *   b       Run a baseline (idle) power test (10s).
 *   e       Prompt for an optional one-line comment, then arm the
 *           energy-usage test; SPACE starts/stops capturing. The
 *           comment (if any) is attached to that one energy-test
 *           result only and cleared afterward.
 *   space   Start/stop the energy capture.
 *   r       Reset the rolling sparkline history.
 *   q       Quit.
 */
#ifndef JEU_TUI_H
#define JEU_TUI_H

#include "ina3221.h"
#include "sampler.h"
#include "sysmon.h"

/* Runs the TUI until the user quits ('q') or SIGINT. Owns dev/sampler/
 * sysmon for the duration (starts/stops them internally, same lifecycle
 * as main.c's --headless path). `use_color` enables per-metric truecolor
 * sparkline gradients (see sparkline.h's SparklineColorScheme) matching
 * the old Python TUI's per-metric colors; pass 0 for plain glyphs only
 * (e.g. for terminals/multiplexers with poor truecolor support, or piped
 * output). Returns 0 on clean exit. */
int run_tui(Ina3221 *dev, Sampler *sampler, SysMonitor *sysmon, int channel, int use_color);

#endif /* JEU_TUI_H */
