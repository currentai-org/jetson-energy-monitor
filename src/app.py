"""
Jetson Energy Usage

A tool for running energy-efficiency tests on the Jetson Orin Nano via its
onboard INA3221 current monitor, with live system resource context (CPU
load, GPU utilization, RAM/swap usage, fan speed, die temperature) via
jetson-stats (jtop). Two front ends share the same test logic:

  1. A Textual TUI (default) with live sparklines and a scrolling log.
  2. A headless CLI mode (--headless baseline|energy) with no TUI at all --
     no live sensor readouts printed to the screen, just a short status
     line while the test runs and a human-readable summary at the end.
     Intended for scripting/cron/low-overhead use, especially alongside
     the user's own application on the same device. See `run_headless()`
     below; Textual is never imported in this mode.

TUI keybindings:
  b        Run a baseline (idle) power test: averages current over 10s.
  e        Arm the energy-usage test. Press SPACE to start capturing,
           press SPACE again to stop; result (mWh/mAh) is reported.
  space    Start/stop the energy capture (only while armed, or toggles
           an already-active capture -- see below).
  r        Reset the rolling sparkline history.
  q        Quit.

Headless CLI mode:
  --headless baseline   Waits (idle) for --duration seconds (default 10s)
                         collecting current samples, then prints results.
  --headless energy     Starts capturing immediately; press any key to
                         stop and print results. Requires an interactive
                         terminal for the keypress -- falls back to
                         Ctrl-C if stdin isn't a tty.
  Both modes still write the same per-sample CSV, sysinfo CSV, and
  results.jsonl as the TUI (see csv_log.py / jsonl_log.py) -- only the
  live/interactive presentation differs.

Architecture:
  - `Sampler` (sampler.py) runs a dedicated background thread reading the
    INA3221 continuously at ~1kHz regardless of what the UI is doing.
  - `SysMonitor` (sysinfo.py) runs a second background thread polling jtop
    (default 2 Hz -- jtop's own service publishes at ~1Hz internally, no
    value in polling faster) for CPU/GPU/RAM/swap/fan/temp.
  - The TUI polls both on a lightweight Textual timer (default 5 Hz screen
    refresh -- deliberately modest; this is a "rough picture" tool, not a
    scope) to update the sparklines and numeric readouts. This refresh
    rate is independent of and never gates either background sampling
    rate. The headless CLI mode polls at a similar cadence purely to keep
    the sampler's bounded buffer drained during a long capture -- it does
    not render anything from those polls.
  - Live visuals use Textual's built-in `Sparkline` widget (single-row,
    unicode block-character bars -- the same style jtop/jetson-stats uses
    in its curses UI) instead of a full plotting library. This was a
    deliberate downgrade from an earlier plotext-based implementation:
    plotext's per-frame rendering of thousands of high-resolution data
    points was pegging a CPU core and the underlying sample buffer peek
    was silently O(buffer size) instead of O(window size), causing the UI
    to visibly bog down/hang after ~15s of runtime. Sparkline only ever
    holds a small fixed-length rolling window (see SPARKLINE_HISTORY) and
    renders in O(width) terminal cells, not O(data points).
  - Blocking test logic (baseline test's 10s wait) runs in a Textual
    `work`-decorated worker (thread) so the UI stays responsive.
  - CPU-reduction pass (see project plan): `dev_last_voltage()` used to do a
    second blocking I2C transaction every UI tick (bus voltage, on top of
    whatever the sampler thread is doing for current). Bus voltage on a
    stiff supply doesn't need 5Hz resolution, so real reads are now
    throttled to VOLTAGE_POLL_HZ (1Hz default) with the cached value
    returned in between -- removes one blocking I2C call per tick from the
    thread that's supposed to stay light so the sampler can get the GIL
    promptly.
  - Textual (and every TUI-specific class -- StatusPanel/SysPanel/EnergyApp)
    is imported and defined *inside* `run_tui()`, not at module scope. This
    means `--headless` mode never imports Textual at all: lower import/
    startup overhead, and one less dependency exercised when this tool is
    running unattended (cron, alongside the user's real application).
"""
from __future__ import annotations

import argparse
import contextlib
import select
import sys
import time

from ina3221 import CHANNEL_NAMES, INA3221, channel_from_name, channel_name
from sampler import Sampler
from sysinfo import SysMonitor
from tests import EnergyCapture, TestResult, run_baseline_test

UI_REFRESH_HZ = 5.0  # deliberately modest -- a "rough picture" tool, not a scope
SYS_POLL_HZ = 2.0
SPARKLINE_HISTORY = 120  # data points retained per sparkline (~24s at 5Hz refresh)
VOLTAGE_POLL_HZ = 1.0  # how often to re-read bus voltage (see dev_last_voltage)
HEADLESS_POLL_HZ = 10.0  # how often the headless CLI mode drains/polls, not renders


def run_tui(
    channel: int,
    target_hz: float,
    sys_poll_hz: float,
    show_plots: bool,
) -> None:
    """Builds and runs the Textual TUI. All Textual imports and widget/App
    class definitions live in this function so --headless mode (see
    run_headless below) never imports Textual at all."""
    from collections import deque

    from textual.app import App, ComposeResult
    from textual.containers import Horizontal, Vertical
    from textual.reactive import reactive
    from textual.widgets import Footer, Header, Log, Sparkline, Static

    from sysinfo import SysSnapshot

    class StatusPanel(Static):
        """Top status line: mode, sample rate, jitter, latest reading."""

        def render_status(
            self,
            mode: str,
            channel_label: str,
            latest_ma: float | None,
            latest_v: float | None,
            achieved_hz: float,
            jitter_us: float,
            max_period_us: float,
            running: bool = False,
        ) -> str:
            if latest_ma is None:
                reading = "-- mA"
            else:
                reading = f"{latest_ma:7.1f} mA"
            v_str = f"{latest_v:.3f} V" if latest_v is not None else "--- V"
            # Pad the plain text first, then wrap in markup -- padding a
            # string that already contains [red]...[/red] tags would count
            # those tag characters towards the width, misaligning the
            # Channel:/Latest: columns that follow.
            mode_padded = f"{mode:<28}"
            mode_str = f"[red]{mode_padded}[/red]" if running else mode_padded
            return (
                f"[b]Mode:[/b] {mode_str} "
                f"[b]Channel:[/b] {channel_label:<12} "
                f"[b]Latest:[/b] {reading}  {v_str}   "
                f"[b]Rate:[/b] {achieved_hz:6.1f} Hz   "
                f"[b]Jitter (std):[/b] {jitter_us:6.1f} us   "
                f"[b]Max period:[/b] {max_period_us:7.1f} us"
            )

    class SysPanel(Static):
        """System resource readout: CPU load, GPU load, RAM, swap, fan, temp."""

        def render_sys(self, s: SysSnapshot) -> str:
            if not s.ok:
                return f"[b]System:[/b] [red]unavailable[/red] -- {s.error or 'no data yet'}"
            fan_str = f"{s.fan_rpm:.0f} RPM" if s.fan_rpm is not None else "--"
            if s.fan_percent is not None:
                fan_str += f" ({s.fan_percent:.0f}%)"
            temp_str = f"{s.temp_c:.1f} C" if s.temp_c is not None else "--"
            return (
                f"[b]CPU:[/b] {s.cpu_percent:5.1f}%   "
                f"[b]GPU:[/b] {s.gpu_percent:5.1f}%   "
                f"[b]RAM:[/b] {s.ram_used_mb:6.0f}/{s.ram_total_mb:.0f} MB ({s.ram_percent:4.1f}%)   "
                f"[b]Swap:[/b] {s.swap_used_mb:6.0f}/{s.swap_total_mb:.0f} MB ({s.swap_percent:4.1f}%)   "
                f"[b]Fan:[/b] {fan_str}   "
                f"[b]Die temp:[/b] {temp_str}"
            )

    class EnergyApp(App):
        TITLE = "Jetson Energy Usage"

        CSS = """
        Screen {
            layout: vertical;
        }
        #status, #sysstatus {
            height: 1;
            padding: 0 1;
            background: $panel;
        }
        .spark-row {
            height: 3;
            margin: 0 1;
        }
        .spark-label {
            width: 16;
            content-align: left top;
        }
        Sparkline {
            width: 1fr;
            height: 3;
        }
        #spark-current > .sparkline--max-color {
            color: yellow;
        }
        #spark-cpu > .sparkline--max-color {
            color: cyan;
        }
        #spark-gpu > .sparkline--max-color {
            color: magenta;
        }
        #spark-temp > .sparkline--max-color {
            color: red;
        }
        #log {
            height: 1fr;
            border: solid $accent;
        }
        /* Textual's default Log text-selection highlight (the
           "screen--selection" component class) uses a semi-transparent
           primary-color background plus an overridden foreground color,
           which can render as an unreadable solid-color block depending
           on the active theme (reported: a green rectangle that swallows
           the text). Override with a background that gives clear contrast
           while leaving the text's own color alone (no `color:` override
           here), so highlighted log lines stay legible. */
        Screen > .screen--selection {
            background: $accent 50%;
        }
        """

        BINDINGS = [
            ("b", "run_baseline", "Baseline test (10s)"),
            ("e", "arm_energy", "Arm energy test"),
            ("space", "toggle_energy", "Start/stop capture"),
            ("r", "reset", "Reset sparklines"),
            ("q", "quit", "Quit"),
        ]

        mode = reactive("idle")

        def __init__(
            self,
            channel: int = 1,
            target_hz: float = 1000.0,
            sys_poll_hz: float = SYS_POLL_HZ,
            show_plots: bool = True,
        ):
            super().__init__()
            self.dev = INA3221()
            self.dev.configure_fast_single_channel(channel=channel)
            self.sampler = Sampler(self.dev, channel=channel, target_hz=target_hz)
            self.sysmon = SysMonitor(poll_hz=sys_poll_hz)
            self.energy = EnergyCapture(self.sampler, self.dev, sysmon=self.sysmon)
            self.show_plots = show_plots
            self._hist_current: deque[float] = deque(maxlen=SPARKLINE_HISTORY)
            self._hist_cpu: deque[float] = deque(maxlen=SPARKLINE_HISTORY)
            self._hist_gpu: deque[float] = deque(maxlen=SPARKLINE_HISTORY)
            self._hist_temp: deque[float] = deque(maxlen=SPARKLINE_HISTORY)
            self._baseline_running = False
            self._last_result: TestResult | None = None
            self._cached_voltage: float | None = None
            self._last_voltage_read_t: float = 0.0

        def compose(self) -> ComposeResult:
            yield Header(show_clock=True)
            yield StatusPanel(id="status")
            yield SysPanel(id="sysstatus")
            if self.show_plots:
                with Horizontal(classes="spark-row"):
                    yield Static("Current (mA):", id="label-current", classes="spark-label")
                    yield Sparkline([], id="spark-current")
                with Horizontal(classes="spark-row"):
                    yield Static("CPU load (%):", id="label-cpu", classes="spark-label")
                    yield Sparkline([], id="spark-cpu")
                with Horizontal(classes="spark-row"):
                    yield Static("GPU load (%):", id="label-gpu", classes="spark-label")
                    yield Sparkline([], id="spark-gpu")
                with Horizontal(classes="spark-row"):
                    yield Static("Die temp (C):", id="label-temp", classes="spark-label")
                    yield Sparkline([], id="spark-temp")
            yield Log(id="log", highlight=False)
            yield Footer()

        def on_mount(self) -> None:
            self.sampler.start()
            self.sysmon.start()
            self.log_widget = self.query_one("#log", Log)
            self.log_widget.write_line("Jetson Energy Usage -- INA3221 sampler started.")
            self.log_widget.write_line(
                "Press 'b' for a 10s baseline test, 'e' then space to start/stop an "
                "energy capture, 'q' to quit."
            )
            if not self.show_plots:
                self.log_widget.write_line("Live sparklines disabled (--no-plots).")
            self.set_interval(1.0 / UI_REFRESH_HZ, self._tick)

        def on_unmount(self) -> None:
            self.sampler.stop()
            self.sysmon.stop()
            self.dev.restore_config()
            self.dev.close()

        def _tick(self) -> None:
            # A test in progress (baseline worker or an active energy capture)
            # owns the sampler's buffer via its own drain()/poll() calls -- we
            # must not also drain here, or we'd steal samples out from under
            # the test and corrupt its results. In that case we only *peek* the
            # single latest value (O(1), no buffer traversal) so the sparkline
            # still shows some movement without touching the buffer.
            testing = self._baseline_running or self.energy.active
            latest_current_ma: float | None = None
            if self.show_plots:
                if testing:
                    latest = self.sampler.latest
                    if latest is not None:
                        self._hist_current.append(latest.current_ma)
                        latest_current_ma = latest.current_ma
                else:
                    # Idle: safe to drain. This also prevents the sample buffer
                    # from silently growing to its 200k-sample cap while no
                    # test is running. One representative value (mean of
                    # whatever accumulated since the last tick, typically ~200
                    # samples at 1kHz/5Hz) keeps this O(samples-per-tick), not
                    # O(sparkline window) or O(buffer size).
                    drained = self.sampler.drain()
                    if drained:
                        mean_ma = sum(s.current_ma for s in drained) / len(drained)
                        self._hist_current.append(mean_ma)
                        latest_current_ma = mean_ma

            # If an energy capture is active, keep draining into it so the
            # sampler's bounded buffer never overflows on a long capture.
            if self.energy.active:
                self.energy.poll()

            stats = self.sampler.stats()
            latest = self.sampler.latest
            status = self.query_one("#status", StatusPanel)
            mode_label = self.mode
            if self.energy.active:
                mode_label = "ENERGY CAPTURE ACTIVE (space to stop)"
            status.update(
                status.render_status(
                    mode=mode_label,
                    channel_label=channel_name(self.sampler.channel),
                    latest_ma=latest.current_ma if latest else None,
                    latest_v=self.dev_last_voltage(),
                    achieved_hz=stats.achieved_hz,
                    jitter_us=stats.jitter_std_us,
                    max_period_us=stats.max_period_us,
                    running=testing,
                )
            )

            sys_snap = self.sysmon.latest
            sysstatus = self.query_one("#sysstatus", SysPanel)
            sysstatus.update(sysstatus.render_sys(sys_snap))
            if self.show_plots and sys_snap.ok:
                self._hist_cpu.append(sys_snap.cpu_percent)
                self._hist_gpu.append(sys_snap.gpu_percent)
                if sys_snap.temp_c is not None:
                    self._hist_temp.append(sys_snap.temp_c)

            if self.show_plots:
                self.query_one("#spark-current", Sparkline).data = list(self._hist_current)
                self.query_one("#spark-cpu", Sparkline).data = list(self._hist_cpu)
                self.query_one("#spark-gpu", Sparkline).data = list(self._hist_gpu)
                self.query_one("#spark-temp", Sparkline).data = list(self._hist_temp)

                cur_str = f"{latest_current_ma:.1f}" if latest_current_ma is not None else "--"
                self.query_one("#label-current", Static).update(f"Current (mA):\n{cur_str}")

                cpu_str = f"{sys_snap.cpu_percent:.1f}" if sys_snap.ok else "--"
                self.query_one("#label-cpu", Static).update(f"CPU load (%):\n{cpu_str}")

                gpu_str = f"{sys_snap.gpu_percent:.1f}" if sys_snap.ok else "--"
                self.query_one("#label-gpu", Static).update(f"GPU load (%):\n{gpu_str}")

                temp_str = (
                    f"{sys_snap.temp_c:.1f}" if sys_snap.ok and sys_snap.temp_c is not None else "--"
                )
                self.query_one("#label-temp", Static).update(f"Die temp (C):\n{temp_str}")

        def dev_last_voltage(self) -> float | None:
            # CPU-reduction: this used to do a blocking I2C read every UI tick
            # (5Hz), which is a second bus transaction competing for the GIL
            # against the sampler thread on top of whatever it's already doing.
            # Bus voltage on a stiff supply doesn't need 5Hz resolution (see
            # README "Known limitations"), so we throttle real reads to
            # VOLTAGE_POLL_HZ and return the cached value between reads.
            now = time.perf_counter()
            if (
                now - self._last_voltage_read_t < 1.0 / VOLTAGE_POLL_HZ
                and self._cached_voltage is not None
            ):
                return self._cached_voltage
            try:
                self._cached_voltage = self.dev.read_bus_voltage_v(self.sampler.channel)
            except OSError:
                self._cached_voltage = None
            self._last_voltage_read_t = now
            return self._cached_voltage

        # -- actions --
        def action_run_baseline(self) -> None:
            if self._baseline_running or self.energy.active:
                self.log_widget.write_line("[!] Busy -- finish current test first.")
                return
            self.mode = "BASELINE RUNNING (10s)..."
            self._baseline_running = True
            self.log_widget.write_line("Baseline test started (10s).")
            self.run_worker(self._baseline_worker, thread=True, exclusive=True)

        def _baseline_worker(self) -> None:
            result = run_baseline_test(self.sampler, self.dev, duration_s=10.0, sysmon=self.sysmon)
            self.call_from_thread(self._on_baseline_done, result)

        def _on_baseline_done(self, result: TestResult) -> None:
            self._baseline_running = False
            self.mode = "idle"
            self._last_result = result
            self.log_widget.write_line("Baseline test stopped.")
            for line in result.summary_lines():
                self.log_widget.write_line(line)

        def action_arm_energy(self) -> None:
            if self._baseline_running:
                self.log_widget.write_line("[!] Busy -- baseline test running.")
                return
            if self.energy.active:
                self.log_widget.write_line("[!] Energy capture already active.")
                return
            self.mode = "ENERGY TEST ARMED (space to start)"
            self.log_widget.write_line("Energy test armed. Press SPACE to start capturing.")

        def action_toggle_energy(self) -> None:
            if self._baseline_running:
                return
            if not self.energy.active:
                self.energy.start()
                self.mode = "ENERGY CAPTURE ACTIVE (space to stop)"
                self.log_widget.write_line("Energy capture started.")
            else:
                result = self.energy.stop()
                self.mode = "idle"
                self._last_result = result
                self.log_widget.write_line("Energy capture stopped.")
                for line in result.summary_lines():
                    self.log_widget.write_line(line)

        def action_reset(self) -> None:
            if not self.show_plots:
                self.log_widget.write_line("[!] Sparklines disabled (--no-plots); nothing to reset.")
                return
            self._hist_current.clear()
            self._hist_cpu.clear()
            self._hist_gpu.clear()
            self._hist_temp.clear()
            self.log_widget.write_line("Sparkline history cleared.")

    app = EnergyApp(
        channel=channel,
        target_hz=target_hz,
        sys_poll_hz=sys_poll_hz,
        show_plots=show_plots,
    )
    app.run()


# --- Headless CLI mode: no Textual, no live sensor printing ----------------


@contextlib.contextmanager
def _raw_terminal():
    """Puts stdin into cbreak mode so a single keypress registers
    immediately without waiting for Enter. No-op (falls through) if stdin
    isn't a real interactive tty (e.g. piped input, non-PTY test harness)
    or termios isn't available (non-POSIX platform) -- callers should treat
    a non-tty stdin as "wait for Ctrl-C instead" (see _run_headless_energy).
    """
    if not sys.stdin.isatty():
        yield
        return
    try:
        import termios
        import tty
    except ImportError:  # pragma: no cover - non-POSIX platform
        yield
        return
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        yield
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)


def _key_pressed(timeout: float) -> bool:
    """Non-blocking-ish check: True if stdin has input ready within
    `timeout` seconds. Used both to detect a keypress and, between checks,
    as the polling cadence for draining the sampler's buffer."""
    ready, _, _ = select.select([sys.stdin], [], [], timeout)
    return bool(ready)


def _run_headless_baseline(
    sampler: Sampler, dev: INA3221, sysmon: SysMonitor, duration_s: float, channel_label: str
) -> TestResult:
    print(f"Baseline (idle) test on {channel_label}: collecting for {duration_s:.0f}s...")
    return run_baseline_test(sampler, dev, duration_s=duration_s, sysmon=sysmon)


def _run_headless_energy(
    sampler: Sampler, dev: INA3221, sysmon: SysMonitor, channel_label: str
) -> TestResult:
    capture = EnergyCapture(sampler, dev, sysmon=sysmon)
    capture.start()
    poll_period = 1.0 / HEADLESS_POLL_HZ
    if sys.stdin.isatty():
        print(f"Energy capture started on {channel_label}. Press any key to stop...")
    else:
        print(
            f"Energy capture started on {channel_label}. "
            "stdin is not a terminal -- press Ctrl-C to stop..."
        )
    try:
        with _raw_terminal():
            while not _key_pressed(poll_period):
                capture.poll()
            if sys.stdin.isatty():
                sys.stdin.read(1)  # consume the keypress so it doesn't leak to the shell
    except KeyboardInterrupt:
        pass
    return capture.stop()


def run_headless(
    test: str,
    channel: int,
    target_hz: float,
    sys_poll_hz: float,
    duration_s: float,
) -> None:
    """Runs a single test with no TUI and no live sensor output -- only a
    short status line before the test and a human-readable summary after.
    Still writes the same per-sample CSV, sysinfo CSV, and results.jsonl as
    the TUI (see tests.py / csv_log.py / jsonl_log.py)."""
    dev = INA3221()
    dev.configure_fast_single_channel(channel=channel)
    sampler = Sampler(dev, channel=channel, target_hz=target_hz)
    sysmon = SysMonitor(poll_hz=sys_poll_hz)
    sampler.start()
    sysmon.start()
    channel_label = channel_name(channel)
    try:
        if test == "baseline":
            result = _run_headless_baseline(sampler, dev, sysmon, duration_s, channel_label)
        else:
            result = _run_headless_energy(sampler, dev, sysmon, channel_label)
        print()
        for line in result.summary_lines():
            print(line)
    finally:
        sampler.stop()
        sysmon.stop()
        dev.restore_config()
        dev.close()


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="jetson-energy-usage",
        description="Tool for running energy-efficiency tests on the Jetson Orin Nano "
        "via its onboard INA3221 current monitor, with live system resource context. "
        "Runs the Textual TUI by default; use --headless for a minimal, non-interactive "
        "CLI mode with no live sensor output.",
    )
    parser.add_argument(
        "--sample-hz",
        type=float,
        default=1000.0,
        metavar="HZ",
        help="Target INA3221 current sampling rate in Hz (default: %(default)s). "
        "Measured achievable rate on pocket-infer-6a8f is ~1600 Hz single-channel; "
        "requesting higher than the hardware/I2C bus can sustain will show up as "
        "reduced 'Rate' and increased jitter in the status bar.",
    )
    parser.add_argument(
        "--sysinfo-hz",
        type=float,
        default=SYS_POLL_HZ,
        metavar="HZ",
        help="Poll rate in Hz for system resource stats (CPU/GPU/RAM/swap/fan/temp) "
        "via jetson-stats (jtop) (default: %(default)s). jtop's own service "
        "publishes at roughly 1 Hz internally, so requesting much faster than "
        "that will not increase real data resolution, only add polling overhead.",
    )
    parser.add_argument(
        "--channel",
        type=str,
        default="VDD_IN",
        choices=list(CHANNEL_NAMES.values()),
        metavar="NAME",
        help="INA3221 rail to sample: VDD_IN (total board input power), "
        "VDD_CPU_GPU (combined CPU+GPU+CV rail), or VDD_SOC (SoC rail). "
        "Default: %(default)s.",
    )
    parser.add_argument(
        "--no-plots",
        action="store_true",
        help="TUI mode only: disable the live current and CPU/GPU sparklines entirely "
        "(status bars, keybindings, and CSV/JSONL logging are unaffected). Use this "
        "for the lowest possible UI overhead. Ignored with --headless, which never "
        "renders sparklines or a status bar in the first place.",
    )
    parser.add_argument(
        "--headless",
        type=str,
        default=None,
        choices=["baseline", "energy"],
        metavar="TEST",
        help="Run a single test directly from the command line with no TUI at all -- "
        "no Textual import, no live sensor readouts printed to the screen, just a "
        "short status line while the test runs and a human-readable results summary "
        "at the end. 'baseline' waits --duration seconds (idle) then reports; "
        "'energy' starts capturing immediately and stops on any keypress (falls back "
        "to Ctrl-C if stdin isn't an interactive terminal). Still writes the same "
        "CSV/sysinfo-CSV/JSONL files as the TUI. Omit this flag to run the TUI.",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=10.0,
        metavar="SECONDS",
        help="Duration in seconds for '--headless baseline' (default: %(default)s). "
        "Ignored for '--headless energy' (stops on keypress/Ctrl-C) and for the TUI "
        "(baseline test there is a fixed 10s via the 'b' keybinding).",
    )
    args = parser.parse_args()

    channel = channel_from_name(args.channel)

    if args.headless:
        run_headless(
            test=args.headless,
            channel=channel,
            target_hz=args.sample_hz,
            sys_poll_hz=args.sysinfo_hz,
            duration_s=args.duration,
        )
        return

    run_tui(
        channel=channel,
        target_hz=args.sample_hz,
        sys_poll_hz=args.sysinfo_hz,
        show_plots=not args.no_plots,
    )


if __name__ == "__main__":
    main()
