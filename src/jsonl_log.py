"""
Append-only JSONL log of test results in the user cache directory.

One JSON object per test invocation (one 'b' baseline run == one line, one
complete 'e' start/stop energy capture == one line). Different test types
can carry different fields/schemas in the same file -- consumers should
treat unknown/absent keys as normal rather than assuming a fixed schema,
and can discriminate on the "test_type" field.

Location: $XDG_CACHE_HOME/jetson-energy-usage/results.jsonl, falling back
to ~/.cache/jetson-energy-usage/results.jsonl (same cache dir as the
per-sample CSVs from csv_log.py).
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from csv_log import cache_dir

RESULTS_FILENAME = "results.jsonl"


def results_log_path() -> Path:
    return cache_dir() / RESULTS_FILENAME


def append_result(record: dict[str, Any]) -> Path:
    """Appends one JSON object (as a single line) to the shared results log.
    `record` should already be JSON-serializable (str/int/float/bool/None/
    list/dict) -- callers are responsible for converting things like Path
    objects to str before calling this.
    """
    path = results_log_path()
    with path.open("a") as f:
        f.write(json.dumps(record, sort_keys=False) + "\n")
    return path
