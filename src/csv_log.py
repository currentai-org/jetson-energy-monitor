"""
CSV logging of raw samples captured during a test run.

Every test invocation (baseline or energy capture) writes its raw
per-sample data to a timestamped CSV in the user cache directory, so runs
can be reviewed/replotted later outside the TUI.

Cache location follows XDG conventions: $XDG_CACHE_HOME/jetson-energy-usage
if set, else ~/.cache/jetson-energy-usage.
"""
from __future__ import annotations

import csv
import os
import time
from pathlib import Path

from sampler import Sample


def cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME")
    root = Path(base) if base else Path.home() / ".cache"
    d = root / "jetson-energy-usage"
    d.mkdir(parents=True, exist_ok=True)
    return d


def write_samples_csv(test_name: str, samples: list[Sample], bus_voltage_v: float | None = None) -> Path:
    """Writes one row per sample: elapsed_s (relative to first sample),
    current_ma, and (if provided) an instantaneous mW estimate using the
    single end-of-window bus voltage reading (same assumption the energy
    integration uses -- see tests.py). Filename is timestamped to the
    second, with a slug of the test name, so concurrent/rapid tests never
    collide.
    """
    slug = "".join(c if c.isalnum() or c in "-_" else "_" for c in test_name.lower())
    ts = time.strftime("%Y%m%d_%H%M%S")
    path = cache_dir() / f"{ts}_{slug}.csv"

    t0 = samples[0].t if samples else 0.0
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        header = ["sample_index", "elapsed_s", "current_ma"]
        if bus_voltage_v is not None:
            header.append("power_mw")
        writer.writerow(header)
        for i, s in enumerate(samples):
            row = [i, f"{s.t - t0:.6f}", f"{s.current_ma:.3f}"]
            if bus_voltage_v is not None:
                row.append(f"{s.current_ma * bus_voltage_v:.3f}")
            writer.writerow(row)

    return path
