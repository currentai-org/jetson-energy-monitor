/*
 * jsonl_log.h -- appends one JSON object per line to the shared
 * results.jsonl in the cache dir, matching src/jsonl_log.py's format and
 * src/tests.py's _log_result_jsonl() field set exactly, so records from
 * the native and Python implementations interleave in the same file with
 * a consistent schema.
 */
#ifndef JEU_JSONL_LOG_H
#define JEU_JSONL_LOG_H

#include "csv_log.h"
#include "ina3221.h"
#include "sampler.h"
#include "sysmon.h"

typedef struct {
    const char *test_type; /* "baseline" or "energy" */
    const char *name;      /* human-readable test name */
    double duration_s;
    long n_samples;
    double mean_current_ma;
    double min_current_ma;
    double max_current_ma;
    double achieved_hz;
    int has_bus_voltage;
    double bus_voltage_v;
    int has_mean_power;
    double mean_power_mw;
    int has_energy;
    double mwh;
    double mah;
    const char *csv_path; /* NULL if none */

    double sampler_target_hz;
    double sampler_mean_period_us;
    double sampler_max_period_us;
    double sampler_jitter_std_us;

    int i2c_channel;
    const char *i2c_channel_name;
    double shunt_ohms;
    int i2c_address;

    const char *sysinfo_csv_path; /* NULL if none */
    int has_sysinfo_summary;
    SysSummaryStats sysinfo_summary;

    /* Extra field only used by baseline: the requested duration, which
     * may differ slightly from the achieved duration_s. -1 to omit. */
    double requested_duration_s;
} JsonlRecordInput;

/* Appends one JSON line to <cache_dir>/results.jsonl. Returns a malloc'd
 * path string (caller frees) on success, NULL on failure. */
char *append_result_jsonl(const JsonlRecordInput *in);

#endif /* JEU_JSONL_LOG_H */
