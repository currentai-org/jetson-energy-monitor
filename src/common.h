/*
 * common.h -- shared types/utilities for the native (pure-C) Jetson Energy
 * Usage CLI. See ../README.md and README.md in this directory for context:
 * this is a from-scratch C reimplementation of the Python --headless CLI
 * mode (src/app.py's run_headless()), aiming for the ina_bench.c
 * benchmark's CPU-footprint floor while reproducing the same baseline/
 * energy test workflow, CSV/JSONL output, and human-readable summary.
 */
#ifndef JEU_COMMON_H
#define JEU_COMMON_H

#include <stddef.h>

/* One current-sensor reading. `t` is a CLOCK_MONOTONIC timestamp in
 * seconds (arbitrary epoch, only differences are meaningful) -- mirrors
 * sampler.py's Sample dataclass (t, current_ma). */
typedef struct {
    double t;
    double current_ma;
} Sample;

/* Growable array of Sample, used by the test-level code (run_baseline_test /
 * EnergyCapture) to accumulate an entire test's samples across repeated
 * Sampler drains -- mirrors Python's `list[Sample]` accumulation in
 * tests.py. Grows by doubling; never shrinks during a test. */
typedef struct {
    Sample *data;
    size_t len;
    size_t cap;
} SampleList;

void sample_list_init(SampleList *list);
void sample_list_free(SampleList *list);
void sample_list_append(SampleList *list, Sample s);
void sample_list_extend(SampleList *list, const Sample *src, size_t n);

/* Cache directory: $XDG_CACHE_HOME/jetson-energy-usage, falling back to
 * ~/.cache/jetson-energy-usage -- mirrors csv_log.py's cache_dir(). Result
 * is a newly malloc'd string (caller frees); the directory is created
 * (mkdir -p semantics) if it doesn't already exist. Returns NULL on
 * failure (couldn't determine HOME, or mkdir failed for a reason other
 * than "already exists"). */
char *jeu_cache_dir(void);

/* mkdir -p equivalent for the (already-absolute) path `path`. Returns 0 on
 * success (including "already exists"), -1 on failure. */
int jeu_mkdir_p(const char *path);

#endif /* JEU_COMMON_H */
