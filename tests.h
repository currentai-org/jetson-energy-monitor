/*
 * tests.h -- baseline test + energy capture logic, mirroring
 * src/tests.py's run_baseline_test() and EnergyCapture class exactly
 * (same stats computation, same trapezoidal mWh/mAh integration, same
 * CSV/JSONL output).
 */
#ifndef JEU_TESTS_H
#define JEU_TESTS_H

#include "common.h"
#include "ina3221.h"
#include "sampler.h"
#include "sysmon.h"

typedef struct {
    char name[64];
    double duration_s;
    long n_samples;
    double mean_current_ma;
    double min_current_ma;
    double max_current_ma;
    double achieved_hz;
    double bus_voltage_v;
    double mean_power_mw;
    int has_energy;
    double mwh;
    double mah;
    int has_comment;
    char comment[256]; /* user-supplied note about this test run, e.g. from
                           the TUI's 'e'-triggered prompt or the headless
                           CLI's --comment flag; empty/has_comment=0 if
                           none was given. */
    char *csv_path;         /* malloc'd, NULL if none -- caller frees */
    char *sysinfo_csv_path; /* malloc'd, NULL if none -- caller frees */
    char *jsonl_path;       /* malloc'd, NULL if none -- caller frees */
    int has_sysinfo_summary;
    SysSummaryStats sysinfo_summary;
} TestResult;

void test_result_free(TestResult *r);

/* Prints the same human-readable summary lines tests.py's
 * TestResult.summary_lines() produces, one per printf call, matching
 * exact spacing/format. */
void test_result_print_summary(const TestResult *r);

/* Blocking: watches the clock for `duration_s` seconds, polling `sysmon`
 * (if non-NULL) and letting samples accumulate in `sampler`'s ring
 * buffer, then drains and summarizes. Mirrors tests.py's
 * run_baseline_test(). Writes CSV/sysinfo-CSV/JSONL as a side effect.
 * `requested_duration_s` is logged in the JSONL record for reference.
 * `comment` (may be NULL or empty) is copied into the result and JSONL
 * record verbatim -- a one-off note about this specific run, e.g. from
 * the headless CLI's --comment flag. */
void run_baseline_test(Sampler *sampler, Ina3221 *dev, SysMonitor *sysmon, double duration_s,
                       const char *comment, TestResult *out);

/* --- EnergyCapture: start/stop-triggered capture, mirrors tests.py's
 * EnergyCapture class. --- */
typedef struct {
    Sampler *sampler;
    Ina3221 *dev;
    SysMonitor *sysmon; /* may be NULL */
    int active;
    SampleList samples;
    SysRecorder sys_rec;
    int has_sys_rec;
    int has_comment;
    char comment[256]; /* set at energy_capture_start() time, consumed by
                           the matching energy_capture_stop()'s TestResult
                           -- callers are expected to supply a fresh
                           comment (or NULL) each time a capture starts,
                           since there's no other lifecycle hook to clear
                           it "after one use". */
} EnergyCapture;

void energy_capture_init(EnergyCapture *ec, Sampler *sampler, Ina3221 *dev, SysMonitor *sysmon);
void energy_capture_free(EnergyCapture *ec);
/* `comment` (may be NULL or empty) is attached to whatever TestResult
 * the *next* energy_capture_stop() call on this EnergyCapture produces --
 * see the field comment above. */
void energy_capture_start(EnergyCapture *ec, const char *comment);
/* Call periodically while active to drain the sampler and poll sysinfo
 * (keeps the sampler's ring buffer from overflowing on a long capture). */
void energy_capture_poll(EnergyCapture *ec);
/* Stops the capture, computes final stats/integration, writes CSV/
 * sysinfo-CSV/JSONL, and fills `out`. */
void energy_capture_stop(EnergyCapture *ec, TestResult *out);

#endif /* JEU_TESTS_H */
