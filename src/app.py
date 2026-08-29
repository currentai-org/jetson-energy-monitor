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
  r        Reset chart / clear last result.
  q        Quit.

Architecture:
  - `Sampler` (sampler.py) runs a dedicated background thread reading the
    INA3221 continuously at ~1kHz regardless of what the UI is doing.
  - `SysMonitor` (sysinfo.py) runs a second background thread polling jtop
    (default 2 Hz -- jtop's own service publishes at ~1Hz internally, no
    value in polling faster) for CPU/GPU/RAM/swap/fan/temp.
  - The UI polls both on a lightweight Textual timer (default 10 Hz screen
    refresh) to update the live charts and readouts -- this refresh rate is
    independent of and never gates either background sampling rate.
  - Blocking test logic (baseline test's 10s wait) runs in a Textual
    `work`-decorated worker (thread) so the UI stays responsive.
"""
from __future__ import annotations

import time
from collections import deque

from textual.app import App, ComposeResult
from textual.containers import Vertical, Horizontal
from textual.reactive import reactive
from textual.widgets import Footer, Header, Static, Log
from textual.worker import Worker, WorkerState
from textual_plotext import PlotextPlot

from ina3221 import INA3221
from sampler import Sampler
from sysinfo import SysMonitor, SysSnapshot
from tests import EnergyCapture, TestResult, run_baseline_test

CHART_WINDOW_S = 15.0  # seconds of history shown on the current chart
SYS_CHART_WINDOW_S = 60.0  # seconds of history shown on the CPU/GPU chart
UI_REFRESH_HZ = 12.0
SYS_POLL_HZ = 2.0


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
    #charts {
        height: 1fr;
    }
    #chart, #syschart {
        width: 1fr;
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
        ("r", "reset", "Reset chart"),
        ("q", "quit", "Quit"),
    ]

    mode = reactive("idle")

    def __init__(self, channel: int = 1, target_hz: float = 1000.0):
        super().__init__()
        self.dev = INA3221()
        self.dev.configure_fast_single_channel(channel=channel)
        self.sampler = Sampler(self.dev, channel=channel, target_hz=target_hz)
        self.energy = EnergyCapture(self.sampler, self.dev)
        self.sysmon = SysMonitor(poll_hz=SYS_POLL_HZ)
        self._chart_t: deque[float] = deque()
        self._chart_ma: deque[float] = deque()
        self._sys_t: deque[float] = deque()
        self._sys_cpu: deque[float] = deque()
        self._sys_gpu: deque[float] = deque()
        self._t0 = time.perf_counter()
        self._baseline_running = False
        self._last_result: TestResult | None = None
        self._last_sys_t_seen: float = 0.0

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield StatusPanel(id="status")
        yield SysPanel(id="sysstatus")
        with Horizontal(id="charts"):
            yield PlotextPlot(id="chart")
            yield PlotextPlot(id="syschart")
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
        self.set_interval(1.0 / UI_REFRESH_HZ, self._tick)

    def on_unmount(self) -> None:
        self.sampler.stop()
        self.sysmon.stop()
        self.dev.restore_config()
        self.dev.close()

    def _tick(self) -> None:
        # Pull recent samples for charting without disturbing the sampler's
        # own buffer bookkeeping used by baseline/energy tests (peek only).
        recent = self.sampler.peek_recent(int(1000 * CHART_WINDOW_S))
        now = time.perf_counter() - self._t0
        for s in recent:
            t_rel = s.t - self._t0
            self._chart_t.append(t_rel)
            self._chart_ma.append(s.current_ma)
        # Trim to window
        cutoff = now - CHART_WINDOW_S
        while self._chart_t and self._chart_t[0] < cutoff:
            self._chart_t.popleft()
            self._chart_ma.popleft()

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
        if sys_snap.ok and sys_snap.t > self._last_sys_t_seen:
            self._last_sys_t_seen = sys_snap.t
            t_rel = now  # align on the same relative timebase as the current chart
            self._sys_t.append(t_rel)
            self._sys_cpu.append(sys_snap.cpu_percent)
            self._sys_gpu.append(sys_snap.gpu_percent)
            sys_cutoff = now - SYS_CHART_WINDOW_S
            while self._sys_t and self._sys_t[0] < sys_cutoff:
                self._sys_t.popleft()
                self._sys_cpu.popleft()
                self._sys_gpu.popleft()

        self._redraw_chart()
        self._redraw_sys_chart()

    def dev_last_voltage(self) -> float | None:
        # Cheap enough at UI refresh rate (~12Hz) to read directly; the fast
        # sampling loop reads shunt voltage only to avoid this overhead.
        try:
            return self.dev.read_bus_voltage_v(self.sampler.channel)
        except OSError:
            return None

    def _redraw_chart(self) -> None:
        plot = self.query_one("#chart", PlotextPlot)
        plt = plot.plt
        plt.clear_data()
        plt.title("Instantaneous current (mA) -- last %.0fs" % CHART_WINDOW_S)
        plt.xlabel("t (s)")
        plt.ylabel("mA")
        if self._chart_t:
            plt.plot(list(self._chart_t), list(self._chart_ma))
        plot.refresh()

    def _redraw_sys_chart(self) -> None:
        plot = self.query_one("#syschart", PlotextPlot)
        plt = plot.plt
        plt.clear_data()
        plt.title("CPU / GPU load (%%) -- last %.0fs" % SYS_CHART_WINDOW_S)
        plt.xlabel("t (s)")
        plt.ylabel("%")
        plt.ylim(0, 100)
        if self._sys_t:
            plt.plot(list(self._sys_t), list(self._sys_cpu), label="CPU", color="cyan")
            plt.plot(list(self._sys_t), list(self._sys_gpu), label="GPU", color="magenta")
        plot.refresh()

    # -- actions --
    def action_run_baseline(self) -> None:
        if self._baseline_running or self.energy.active:
            self.log_widget.write_line("[!] Busy -- finish current test first.")
            return
        self.mode = "BASELINE RUNNING (10s)..."
        self._baseline_running = True
        self.run_worker(self._baseline_worker, thread=True, exclusive=True)

    def _baseline_worker(self) -> None:
        result = run_baseline_test(self.sampler, self.dev, duration_s=10.0)
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
        self._chart_t.clear()
        self._chart_ma.clear()
        self._sys_t.clear()
        self._sys_cpu.clear()
        self._sys_gpu.clear()
        self.log_widget.write_line("Charts cleared.")


def main() -> None:
    app = EnergyApp()
    app.run()


if __name__ == "__main__":
    main()
