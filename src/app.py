"""
Jetson Energy Usage TUI

A Textual-based terminal UI for running energy-efficiency tests on the
Jetson Orin Nano via its onboard INA3221 current monitor, with live system
resource context (CPU load, GPU utilization, RAM/swap usage, fan speed,
die temperature) via jetson-stats (jtop).

Keybindings:
  b        Run a baseline (idle) power test: averages current over 10s.
  e        Arm the energy-usage test. Press SPACE to start capturing,
           press SPACE again to stop; result (mWh/mAh) is reported.
  space    Start/stop the energy capture (only while armed, or toggles
           an already-active capture -- see below).
  r        Reset the rolling sparkline history.
  q        Quit.

Architecture:
  - `Sampler` (sampler.py) runs a dedicated background thread reading the
    INA3221 continuously at ~1kHz regardless of what the UI is doing.
  - `SysMonitor` (sysinfo.py) runs a second background thread polling jtop
    (default 2 Hz -- jtop's own service publishes at ~1Hz internally, no
    value in polling faster) for CPU/GPU/RAM/swap/fan/temp.
  - The UI polls both on a lightweight Textual timer (default 5 Hz screen
    refresh -- deliberately modest; this is a "rough picture" tool, not a
    scope) to update the sparklines and numeric readouts. This refresh
    rate is independent of and never gates either background sampling
    rate.
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
"""
from __future__ import annotations

import argparse
import time
from collections import deque

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.reactive import reactive
from textual.widgets import Footer, Header, Log, Sparkline, Static

from ina3221 import INA3221
from sampler import Sampler
from sysinfo import SysMonitor, SysSnapshot
from tests import EnergyCapture, TestResult, run_baseline_test

UI_REFRESH_HZ = 5.0  # deliberately modest -- a "rough picture" tool, not a scope
SYS_POLL_HZ = 2.0
SPARKLINE_HISTORY = 120  # data points retained per sparkline (~24s at 5Hz refresh)


class StatusPanel(Static):
    """Top status line: mode, sample rate, jitter, latest reading."""

    def render_status(
        self,
        mode: str,
        latest_ma: float | None,
        latest_v: float | None,
        achieved_hz: float,
        jitter_us: float,
        max_period_us: float,
    ) -> str:
        if latest_ma is None:
            reading = "-- mA"
        else:
            reading = f"{latest_ma:7.1f} mA"
        v_str = f"{latest_v:.3f} V" if latest_v is not None else "--- V"
        return (
            f"[b]Mode:[/b] {mode:<28} "
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
    CSS = """
    Screen {
        layout: vertical;
    }
    #status, #sysstatus {
        height: 3;
        padding: 1;
        background: $panel;
    }
    .spark-row {
        height: 1;
        margin: 0 1;
    }
    .spark-label {
        width: 16;
    }
    Sparkline {
        width: 1fr;
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
    #log {
        height: 10;
        border: solid $accent;
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
        self._baseline_running = False
        self._last_result: TestResult | None = None

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield StatusPanel(id="status")
        yield SysPanel(id="sysstatus")
        if self.show_plots:
            with Horizontal(classes="spark-row"):
                yield Static("Current (mA):", classes="spark-label")
                yield Sparkline([], id="spark-current")
            with Horizontal(classes="spark-row"):
                yield Static("CPU load (%):", classes="spark-label")
                yield Sparkline([], id="spark-cpu")
            with Horizontal(classes="spark-row"):
                yield Static("GPU load (%):", classes="spark-label")
                yield Sparkline([], id="spark-gpu")
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
        if self.show_plots:
            if testing:
                latest = self.sampler.latest
                if latest is not None:
                    self._hist_current.append(latest.current_ma)
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
                latest_ma=latest.current_ma if latest else None,
                latest_v=self.dev_last_voltage(),
                achieved_hz=stats.achieved_hz,
                jitter_us=stats.jitter_std_us,
                max_period_us=stats.max_period_us,
            )
        )

        sys_snap = self.sysmon.latest
        sysstatus = self.query_one("#sysstatus", SysPanel)
        sysstatus.update(sysstatus.render_sys(sys_snap))
        if self.show_plots and sys_snap.ok:
            self._hist_cpu.append(sys_snap.cpu_percent)
            self._hist_gpu.append(sys_snap.gpu_percent)

        if self.show_plots:
            self.query_one("#spark-current", Sparkline).data = list(self._hist_current)
            self.query_one("#spark-cpu", Sparkline).data = list(self._hist_cpu)
            self.query_one("#spark-gpu", Sparkline).data = list(self._hist_gpu)

    def dev_last_voltage(self) -> float | None:
        # Cheap enough at UI refresh rate (5Hz) to read directly; the fast
        # sampling loop reads shunt voltage only to avoid this overhead.
        try:
            return self.dev.read_bus_voltage_v(self.sampler.channel)
        except OSError:
            return None

    # -- actions --
    def action_run_baseline(self) -> None:
        if self._baseline_running or self.energy.active:
            self.log_widget.write_line("[!] Busy -- finish current test first.")
            return
        self.mode = "BASELINE RUNNING (10s)..."
        self._baseline_running = True
        self.run_worker(self._baseline_worker, thread=True, exclusive=True)

    def _baseline_worker(self) -> None:
        result = run_baseline_test(self.sampler, self.dev, duration_s=10.0, sysmon=self.sysmon)
        self.call_from_thread(self._on_baseline_done, result)

    def _on_baseline_done(self, result: TestResult) -> None:
        self._baseline_running = False
        self.mode = "idle"
        self._last_result = result
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
        self.log_widget.write_line("Sparkline history cleared.")


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="jetson-energy-usage",
        description="TUI for running energy-efficiency tests on the Jetson Orin Nano "
        "via its onboard INA3221 current monitor, with live system resource context.",
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
        type=int,
        default=1,
        choices=[1, 2, 3],
        help="INA3221 channel to sample (1=VDD_IN total board power, "
        "2=VDD_CPU_GPU_CV, 3=VDD_SOC). Default: %(default)s.",
    )
    parser.add_argument(
        "--no-plots",
        action="store_true",
        help="Disable the live current and CPU/GPU sparklines entirely (status bars, "
        "keybindings, and CSV/JSONL logging are unaffected). Use this for the "
        "lowest possible UI overhead.",
    )
    args = parser.parse_args()

    app = EnergyApp(
        channel=args.channel,
        target_hz=args.sample_hz,
        sys_poll_hz=args.sysinfo_hz,
        show_plots=not args.no_plots,
    )
    app.run()


if __name__ == "__main__":
    main()
