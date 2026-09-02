/*
 * csv_log.h -- writes per-sample CSV and sysinfo CSV files, matching
 * src/csv_log.py's format exactly (same columns, same filename
 * convention: <cache_dir>/<timestamp>_<slug>.csv and
 * <cache_dir>/<timestamp>_<slug>_sysinfo.csv).
 */
#ifndef JEU_CSV_LOG_H
#define JEU_CSV_LOG_H

#include "common.h"
#include "sysmon.h"

/* Writes samples[0..n) to a timestamped CSV in the cache dir. If
 * bus_voltage_v >= 0, an extra power_mw column is included (current_ma *
 * bus_voltage_v per row, matching csv_log.py). Returns a malloc'd path
 * string (caller frees) on success, NULL on failure. */
char *write_samples_csv(const char *test_name, const Sample *samples, size_t n,
                         double bus_voltage_v_or_negative);

/* Writes sys_samples[0..n) to a timestamped "_sysinfo" CSV. Returns NULL
 * (writes nothing) if n == 0, matching csv_log.py's None-on-empty
 * behavior. Returns a malloc'd path string on success. */
char *write_sysinfo_csv(const char *test_name, const SysSnapshot *sys_samples, size_t n);

#endif /* JEU_CSV_LOG_H */
