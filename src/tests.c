/* tests.c -- see tests.h */
#include "tests.h"

#include "csv_log.h"
#include "jsonl_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Mirrors tests.py's _stats_from_samples(): returns via out params
 * (mean_ma, min_ma, max_ma, duration_s). */
static void stats_from_samples(const Sample *samples, size_t n, double *mean_ma, double *min_ma,
                                double *max_ma, double *duration_s) {
    if (n == 0) {
        *mean_ma = *min_ma = *max_ma = *duration_s = 0.0;
        return;
    }
    double sum = 0.0, mn = samples[0].current_ma, mx = samples[0].current_ma;
    for (size_t i = 0; i < n; i++) {
        sum += samples[i].current_ma;
        if (samples[i].current_ma < mn) mn = samples[i].current_ma;
        if (samples[i].current_ma > mx) mx = samples[i].current_ma;
    }
    *mean_ma = sum / n;
    *min_ma = mn;
    *max_ma = mx;
    *duration_s = samples[n - 1].t - samples[0].t;
}

/* Mirrors tests.py's integrate_mwh(): trapezoidal integration of current
 * over time -> (mah, mwh) via out params. */
static void integrate_mwh(const Sample *samples, size_t n, double bus_voltage_v, double *mah_out,
                          double *mwh_out) {
    if (n < 2) {
        *mah_out = 0.0;
        *mwh_out = 0.0;
        return;
    }
    double mah = 0.0;
    for (size_t i = 1; i < n; i++) {
        double dt_h = (samples[i].t - samples[i - 1].t) / 3600.0;
        double avg_ma = (samples[i].current_ma + samples[i - 1].current_ma) / 2.0;
        mah += avg_ma * dt_h;
    }
    *mah_out = mah;
    *mwh_out = mah * bus_voltage_v;
}

void test_result_free(TestResult *r) {
    free(r->csv_path);
    free(r->sysinfo_csv_path);
    free(r->jsonl_path);
    r->csv_path = r->sysinfo_csv_path = r->jsonl_path = NULL;
}

void test_result_print_summary(const TestResult *r) {
    printf("%s\n", r->name);
    if (r->has_comment && r->comment[0]) {
        printf("  comment:       %s\n", r->comment);
    }
    printf("  duration:      %.3f s\n", r->duration_s);
    printf("  samples:       %ld  (~%.0f Hz)\n", r->n_samples, r->achieved_hz);
    printf("  mean current:  %.1f mA\n", r->mean_current_ma);
    printf("  min / max:     %.1f / %.1f mA\n", r->min_current_ma, r->max_current_ma);
    printf("  bus voltage:   %.3f V\n", r->bus_voltage_v);
    printf("  mean power:    %.1f mW\n", r->mean_power_mw);
    if (r->has_energy) {
        printf("  energy:        %.3f mWh  (%.3f mAh)\n", r->mwh, r->mah);
    }
    if (r->has_sysinfo_summary && r->sysinfo_summary.sys_n_samples > 0) {
        const SysSummaryStats *s = &r->sysinfo_summary;
        printf("  CPU avg/max:   %.1f%% / %.1f%%\n", s->avg_cpu_percent, s->max_cpu_percent);
        printf("  GPU avg/max:   %.1f%% / %.1f%%\n", s->avg_gpu_percent, s->max_gpu_percent);
        if (s->has_temp) {
            printf("  die temp avg/max: %.1f C / %.1f C\n", s->avg_temp_c, s->max_temp_c);
        }
    }
    if (r->csv_path) printf("  raw samples:   %s\n", r->csv_path);
    if (r->sysinfo_csv_path) printf("  sysinfo csv:   %s\n", r->sysinfo_csv_path);
    if (r->jsonl_path) printf("  logged to:     %s\n", r->jsonl_path);
}

static void log_result_jsonl(const char *test_type, TestResult *result, Sampler *sampler,
                              Ina3221 *dev, double requested_duration_s) {
    SamplerStats stats;
    sampler_get_stats(sampler, &stats);

    JsonlRecordInput in;
    memset(&in, 0, sizeof(in));
    in.test_type = test_type;
    in.name = result->name;
    in.duration_s = result->duration_s;
    in.n_samples = result->n_samples;
    in.mean_current_ma = result->mean_current_ma;
    in.min_current_ma = result->min_current_ma;
    in.max_current_ma = result->max_current_ma;
    in.achieved_hz = result->achieved_hz;
    in.has_bus_voltage = 1;
    in.bus_voltage_v = result->bus_voltage_v;
    in.has_mean_power = 1;
    in.mean_power_mw = result->mean_power_mw;
    in.has_energy = result->has_energy;
    in.mwh = result->mwh;
    in.mah = result->mah;
    in.has_comment = result->has_comment;
    in.comment = (result->has_comment && result->comment[0]) ? result->comment : NULL;
    in.csv_path = result->csv_path;

    in.sampler_target_hz = 1.0 / sampler->target_period_s;
    in.sampler_mean_period_us = stats.mean_period_us;
    in.sampler_max_period_us = stats.max_period_us;
    in.sampler_jitter_std_us = stats.jitter_std_us;

    in.i2c_channel = sampler->channel;
    in.i2c_channel_name = ina3221_channel_name(sampler->channel);
    in.shunt_ohms = dev->shunt_ohms;
    in.i2c_address = dev->address;

    in.sysinfo_csv_path = result->sysinfo_csv_path;
    in.has_sysinfo_summary = result->has_sysinfo_summary;
    in.sysinfo_summary = result->sysinfo_summary;
    in.requested_duration_s = requested_duration_s;

    result->jsonl_path = append_result_jsonl(&in);
}

void run_baseline_test(Sampler *sampler, Ina3221 *dev, SysMonitor *sysmon, double duration_s,
                       const char *comment, TestResult *out) {
    memset(out, 0, sizeof(*out));

    SampleList discard;
    sample_list_init(&discard);
    sampler_drain(sampler, &discard); /* clear stale samples */
    sample_list_free(&discard);

    SysRecorder sys_rec;
    int have_sys_rec = sysmon != NULL;
    if (have_sys_rec) {
        sys_recorder_init(&sys_rec, sysmon);
        sys_recorder_start(&sys_rec);
    }

    double start = now_monotonic();
    while (now_monotonic() - start < duration_s) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000}; /* 10ms */
        nanosleep(&ts, NULL);
        if (have_sys_rec) sys_recorder_poll(&sys_rec);
    }

    SampleList samples;
    sample_list_init(&samples);
    sampler_drain(sampler, &samples);

    double mean_ma, min_ma, max_ma, dur;
    stats_from_samples(samples.data, samples.len, &mean_ma, &min_ma, &max_ma, &dur);
    double achieved_hz = (dur > 0) ? (double)samples.len / dur : 0.0;

    double bus_v = 0.0;
    ina3221_read_bus_voltage_v(dev, sampler->channel, &bus_v);
    double mean_power_mw = mean_ma * bus_v;

    char *csv_path = write_samples_csv("baseline", samples.data, samples.len, bus_v);
    char *sysinfo_csv_path = NULL;
    if (have_sys_rec) {
        sysinfo_csv_path = write_sysinfo_csv("baseline", sys_rec.samples, sys_rec.len);
    }

    strncpy(out->name, "Baseline (idle) power test", sizeof(out->name) - 1);
    out->duration_s = dur;
    out->n_samples = (long)samples.len;
    out->mean_current_ma = mean_ma;
    out->min_current_ma = min_ma;
    out->max_current_ma = max_ma;
    out->achieved_hz = achieved_hz;
    out->bus_voltage_v = bus_v;
    out->mean_power_mw = mean_power_mw;
    out->has_energy = 0;
    if (comment && comment[0]) {
        out->has_comment = 1;
        snprintf(out->comment, sizeof(out->comment), "%s", comment);
    }
    out->csv_path = csv_path;
    out->sysinfo_csv_path = sysinfo_csv_path;
    if (have_sys_rec) {
        out->has_sysinfo_summary = 1;
        sys_recorder_summary(&sys_rec, &out->sysinfo_summary);
        sys_recorder_free(&sys_rec);
    }

    log_result_jsonl("baseline", out, sampler, dev, duration_s);

    sample_list_free(&samples);
}

void energy_capture_init(EnergyCapture *ec, Sampler *sampler, Ina3221 *dev, SysMonitor *sysmon) {
    memset(ec, 0, sizeof(*ec));
    ec->sampler = sampler;
    ec->dev = dev;
    ec->sysmon = sysmon;
    sample_list_init(&ec->samples);
    ec->has_sys_rec = sysmon != NULL;
    if (ec->has_sys_rec) sys_recorder_init(&ec->sys_rec, sysmon);
}

void energy_capture_free(EnergyCapture *ec) {
    sample_list_free(&ec->samples);
    if (ec->has_sys_rec) sys_recorder_free(&ec->sys_rec);
}

void energy_capture_start(EnergyCapture *ec, const char *comment) {
    SampleList discard;
    sample_list_init(&discard);
    sampler_drain(ec->sampler, &discard);
    sample_list_free(&discard);

    sample_list_free(&ec->samples);
    sample_list_init(&ec->samples);
    ec->active = 1;
    if (comment && comment[0]) {
        ec->has_comment = 1;
        snprintf(ec->comment, sizeof(ec->comment), "%s", comment);
    } else {
        ec->has_comment = 0;
        ec->comment[0] = '\0';
    }
    if (ec->has_sys_rec) sys_recorder_start(&ec->sys_rec);
}

void energy_capture_poll(EnergyCapture *ec) {
    if (!ec->active) return;
    SampleList drained;
    sample_list_init(&drained);
    sampler_drain(ec->sampler, &drained);
    sample_list_extend(&ec->samples, drained.data, drained.len);
    sample_list_free(&drained);
    if (ec->has_sys_rec) sys_recorder_poll(&ec->sys_rec);
}

void energy_capture_stop(EnergyCapture *ec, TestResult *out) {
    memset(out, 0, sizeof(*out));
    ec->active = 0;

    SampleList drained;
    sample_list_init(&drained);
    sampler_drain(ec->sampler, &drained);
    sample_list_extend(&ec->samples, drained.data, drained.len);
    sample_list_free(&drained);

    Sample *samples = ec->samples.data;
    size_t n = ec->samples.len;

    double mean_ma, min_ma, max_ma, dur;
    stats_from_samples(samples, n, &mean_ma, &min_ma, &max_ma, &dur);
    double achieved_hz = (dur > 0) ? (double)n / dur : 0.0;

    double bus_v = 0.0;
    ina3221_read_bus_voltage_v(ec->dev, ec->sampler->channel, &bus_v);
    double mah, mwh;
    integrate_mwh(samples, n, bus_v, &mah, &mwh);
    double mean_power_mw = (dur > 0) ? (mwh / (dur / 3600.0)) : 0.0;

    char *csv_path = write_samples_csv("energy", samples, n, bus_v);
    char *sysinfo_csv_path = NULL;
    if (ec->has_sys_rec) {
        sysinfo_csv_path = write_sysinfo_csv("energy", ec->sys_rec.samples, ec->sys_rec.len);
    }

    strncpy(out->name, "Energy usage test", sizeof(out->name) - 1);
    out->duration_s = dur;
    out->n_samples = (long)n;
    out->mean_current_ma = mean_ma;
    out->min_current_ma = min_ma;
    out->max_current_ma = max_ma;
    out->achieved_hz = achieved_hz;
    out->bus_voltage_v = bus_v;
    out->mean_power_mw = mean_power_mw;
    out->has_energy = 1;
    out->mwh = mwh;
    out->mah = mah;
    if (ec->has_comment && ec->comment[0]) {
        out->has_comment = 1;
        snprintf(out->comment, sizeof(out->comment), "%s", ec->comment);
    }
    out->csv_path = csv_path;
    out->sysinfo_csv_path = sysinfo_csv_path;
    if (ec->has_sys_rec) {
        out->has_sysinfo_summary = 1;
        sys_recorder_summary(&ec->sys_rec, &out->sysinfo_summary);
    }

    log_result_jsonl("energy", out, ec->sampler, ec->dev, -1.0);

    /* Comment is one-shot: clear it now so a subsequent start (without an
     * explicit new comment) doesn't silently reuse this one. */
    ec->has_comment = 0;
    ec->comment[0] = '\0';
}
