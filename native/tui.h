/*
 * tui.h -- interactive terminal UI: sparklines (current/CPU/GPU/temp),
 * status line, sysinfo line, and a small scrolling log -- at a fraction
 * of the CPU cost of the old Textual-based TUI. See README.md's TUI
 * section for the design rationale (raw ANSI + hand-rolled diffing
 * instead of ncurses/Cython).
 *
 * Keybindings (same as the old Python TUI):
 *   b       Run a baseline (idle) power test (10s).
 *   e       Arm the energy-usage test; SPACE starts/stops capturing.
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
 * as main.c's --headless path). Returns 0 on clean exit. */
int run_tui(Ina3221 *dev, Sampler *sampler, SysMonitor *sysmon, int channel);

#endif /* JEU_TUI_H */
