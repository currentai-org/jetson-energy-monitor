/*
 * sysmon.h -- background thread polling sysinfo_read_snapshot() at a
 * fixed rate, exposing the latest snapshot for cheap reads from another
 * thread. Mirrors src/sysinfo.py's SysMonitor class.
 */
#ifndef JEU_SYSMON_H
#define JEU_SYSMON_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>

#include "sysinfo.h"

typedef struct {
    double poll_period_s;
    pthread_t thread;
    volatile sig_atomic_t stop_requested;
    int thread_running;
    pthread_mutex_t lock;
    SysSnapshot latest; /* protected by `lock` */
    ProcStatCpuReader cpu_reader;
} SysMonitor;

void sysmon_init(SysMonitor *m, double poll_hz);
int sysmon_start(SysMonitor *m);
void sysmon_stop(SysMonitor *m);
void sysmon_destroy(SysMonitor *m);

/* Copies the latest snapshot into `out` (thread-safe). */
void sysmon_get_latest(SysMonitor *m, SysSnapshot *out);

/* --- SysRecorder: accumulates a de-duplicated series of SysSnapshots
 * over the course of a test, for later avg/min/max summarization.
 * Mirrors sysinfo.py's SysRecorder. */
typedef struct {
    SysMonitor *sysmon;
    SysSnapshot *samples;
    size_t len;
    size_t cap;
    double last_t_seen;
} SysRecorder;

void sys_recorder_init(SysRecorder *r, SysMonitor *sysmon);
void sys_recorder_free(SysRecorder *r);
void sys_recorder_start(SysRecorder *r);
/* Call periodically while a test is running; only records a new entry
 * when the underlying snapshot's timestamp actually changed. */
void sys_recorder_poll(SysRecorder *r);

typedef struct {
    long sys_n_samples;
    double avg_cpu_percent, min_cpu_percent, max_cpu_percent;
    double avg_gpu_percent, min_gpu_percent, max_gpu_percent;
    double avg_ram_percent, min_ram_percent, max_ram_percent;
    double avg_swap_percent, min_swap_percent, max_swap_percent;
    int has_fan_rpm;
    double avg_fan_rpm, min_fan_rpm, max_fan_rpm;
    int has_temp;
    double avg_temp_c, min_temp_c, max_temp_c;
} SysSummaryStats;

/* Computes avg/min/max across all recorded samples -- mirrors
 * sysinfo.py's SysRecorder.summary_stats(). */
void sys_recorder_summary(SysRecorder *r, SysSummaryStats *out);

#endif /* JEU_SYSMON_H */
