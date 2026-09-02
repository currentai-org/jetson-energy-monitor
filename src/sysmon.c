/* sysmon.c -- see sysmon.h */
#include "sysmon.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

void sysmon_init(SysMonitor *m, double poll_hz) {
    memset(m, 0, sizeof(*m));
    m->poll_period_s = 1.0 / poll_hz;
    pthread_mutex_init(&m->lock, NULL);
    proc_stat_cpu_reader_init(&m->cpu_reader);
    sysinfo_find_fan_paths();
}

static void *sysmon_thread_main(void *arg) {
    SysMonitor *m = (SysMonitor *)arg;
    while (!m->stop_requested) {
        SysSnapshot snap;
        sysinfo_read_snapshot(&m->cpu_reader, &snap);
        pthread_mutex_lock(&m->lock);
        m->latest = snap;
        pthread_mutex_unlock(&m->lock);

        struct timespec ts;
        ts.tv_sec = (time_t)m->poll_period_s;
        ts.tv_nsec = (long)((m->poll_period_s - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int sysmon_start(SysMonitor *m) {
    m->stop_requested = 0;
    int rc = pthread_create(&m->thread, NULL, sysmon_thread_main, m);
    if (rc != 0) return -1;
    m->thread_running = 1;
    return 0;
}

void sysmon_stop(SysMonitor *m) {
    if (!m->thread_running) return;
    m->stop_requested = 1;
    pthread_join(m->thread, NULL);
    m->thread_running = 0;
}

void sysmon_destroy(SysMonitor *m) {
    pthread_mutex_destroy(&m->lock);
}

void sysmon_get_latest(SysMonitor *m, SysSnapshot *out) {
    pthread_mutex_lock(&m->lock);
    *out = m->latest;
    pthread_mutex_unlock(&m->lock);
}

/* --- SysRecorder --- */

void sys_recorder_init(SysRecorder *r, SysMonitor *sysmon) {
    memset(r, 0, sizeof(*r));
    r->sysmon = sysmon;
}

void sys_recorder_free(SysRecorder *r) {
    free(r->samples);
    r->samples = NULL;
    r->len = 0;
    r->cap = 0;
}

void sys_recorder_start(SysRecorder *r) {
    free(r->samples);
    r->samples = NULL;
    r->len = 0;
    r->cap = 0;
    r->last_t_seen = 0.0;
}

static void sys_recorder_ensure_cap(SysRecorder *r, size_t min_cap) {
    if (r->cap >= min_cap) return;
    size_t new_cap = r->cap ? r->cap * 2 : 256;
    while (new_cap < min_cap) new_cap *= 2;
    r->samples = realloc(r->samples, new_cap * sizeof(SysSnapshot));
    r->cap = new_cap;
}

void sys_recorder_poll(SysRecorder *r) {
    SysSnapshot snap;
    sysmon_get_latest(r->sysmon, &snap);
    if (snap.ok && snap.t > r->last_t_seen) {
        r->last_t_seen = snap.t;
        sys_recorder_ensure_cap(r, r->len + 1);
        r->samples[r->len++] = snap;
    }
}

void sys_recorder_summary(SysRecorder *r, SysSummaryStats *out) {
    memset(out, 0, sizeof(*out));
    out->sys_n_samples = (long)r->len;
    if (r->len == 0) return;

    double sum_cpu = 0, sum_gpu = 0, sum_ram = 0, sum_swap = 0;
    double min_cpu = 1e18, max_cpu = -1e18;
    double min_gpu = 1e18, max_gpu = -1e18;
    double min_ram = 1e18, max_ram = -1e18;
    double min_swap = 1e18, max_swap = -1e18;

    double sum_fan = 0, min_fan = 1e18, max_fan = -1e18;
    long fan_n = 0;
    double sum_temp = 0, min_temp = 1e18, max_temp = -1e18;
    long temp_n = 0;

    for (size_t i = 0; i < r->len; i++) {
        SysSnapshot *s = &r->samples[i];
        sum_cpu += s->cpu_percent;
        if (s->cpu_percent < min_cpu) min_cpu = s->cpu_percent;
        if (s->cpu_percent > max_cpu) max_cpu = s->cpu_percent;

        sum_gpu += s->gpu_percent;
        if (s->gpu_percent < min_gpu) min_gpu = s->gpu_percent;
        if (s->gpu_percent > max_gpu) max_gpu = s->gpu_percent;

        sum_ram += s->ram_percent;
        if (s->ram_percent < min_ram) min_ram = s->ram_percent;
        if (s->ram_percent > max_ram) max_ram = s->ram_percent;

        sum_swap += s->swap_percent;
        if (s->swap_percent < min_swap) min_swap = s->swap_percent;
        if (s->swap_percent > max_swap) max_swap = s->swap_percent;

        if (s->fan_rpm >= 0) {
            sum_fan += s->fan_rpm;
            if (s->fan_rpm < min_fan) min_fan = s->fan_rpm;
            if (s->fan_rpm > max_fan) max_fan = s->fan_rpm;
            fan_n++;
        }
        if (s->temp_c > -999.0) {
            sum_temp += s->temp_c;
            if (s->temp_c < min_temp) min_temp = s->temp_c;
            if (s->temp_c > max_temp) max_temp = s->temp_c;
            temp_n++;
        }
    }

    out->avg_cpu_percent = sum_cpu / r->len;
    out->min_cpu_percent = min_cpu;
    out->max_cpu_percent = max_cpu;
    out->avg_gpu_percent = sum_gpu / r->len;
    out->min_gpu_percent = min_gpu;
    out->max_gpu_percent = max_gpu;
    out->avg_ram_percent = sum_ram / r->len;
    out->min_ram_percent = min_ram;
    out->max_ram_percent = max_ram;
    out->avg_swap_percent = sum_swap / r->len;
    out->min_swap_percent = min_swap;
    out->max_swap_percent = max_swap;

    if (fan_n > 0) {
        out->has_fan_rpm = 1;
        out->avg_fan_rpm = sum_fan / fan_n;
        out->min_fan_rpm = min_fan;
        out->max_fan_rpm = max_fan;
    }
    if (temp_n > 0) {
        out->has_temp = 1;
        out->avg_temp_c = sum_temp / temp_n;
        out->min_temp_c = min_temp;
        out->max_temp_c = max_temp;
    }
}
