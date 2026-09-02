/*
 * sysinfo.h -- system resource snapshot (CPU/GPU/RAM/swap/fan/temp) read
 * directly from /proc and /sys, no jtop dependency at all. Mirrors
 * src/sysinfo.py's SysSnapshot fields/semantics, but sourced natively:
 *   - CPU%:  /proc/stat delta (same technique sysinfo.py's
 *            _ProcStatCpuReader uses, for the same reason -- see its
 *            docstring on the jtop-service CPU estimator reset bug this
 *            avoids by construction, since we never touch jtop at all).
 *   - GPU%:  /sys/devices/platform/bus@0/17000000.gpu/load (0-1000 scale,
 *            confirmed against jtop/core/gpu.py's own parsing: `load /
 *            10.0` -> percent).
 *   - RAM/swap: /proc/meminfo (MemTotal/MemAvailable, SwapTotal/SwapFree).
 *   - Fan:   /sys/class/hwmon/hwmon3/rpm (tachometer) and
 *            /sys/devices/platform/pwm-fan/hwmon/hwmon0/pwm1 (0-255 duty
 *            cycle, matching jtop/core/fan.py's FAN_PWM_CAP=255 scaling
 *            to a percent). Both paths were confirmed present on
 *            pocket-infer-6a8f via `find /sys/class/hwmon`; see
 *            sysinfo_find_fan_paths() for the runtime discovery that
 *            makes this robust to a different hwmonN numbering on other
 *            Jetson boards/kernel versions.
 *   - Temp:  /sys/class/thermal/thermal_zoneN/type and .../temp -- picks
 *            the zone whose type is "tj" (thermal junction, effective die
 *            temp used for throttling) if present, else the hottest
 *            reported zone, matching sysinfo.py's SysMonitor.
 */
#ifndef JEU_SYSINFO_H
#define JEU_SYSINFO_H

#define SYSINFO_MAX_TEMP_ZONES 16
#define SYSINFO_ZONE_NAME_LEN 32

typedef struct {
    int ok;
    double cpu_percent;
    double gpu_percent;
    double ram_used_mb;
    double ram_total_mb;
    double ram_percent;
    double swap_used_mb;
    double swap_total_mb;
    double swap_percent;
    double fan_rpm;      /* -1 if unavailable */
    double fan_percent;  /* -1 if unavailable */
    double temp_c;       /* -1000 if unavailable (die/tj or hottest zone) */
    double t;            /* CLOCK_MONOTONIC timestamp of this snapshot */
} SysSnapshot;

/* Per-process /proc/stat CPU delta tracker -- must persist across calls
 * (holds the previous cumulative jiffy counters) so cpu_percent reflects
 * the delta since the last read_snapshot() call, not since boot. */
typedef struct {
    long long prev_total[8]; /* aggregate "cpu" line only; we don't need
                                per-core breakdown for this CLI */
    int has_prev;
} ProcStatCpuReader;

void proc_stat_cpu_reader_init(ProcStatCpuReader *r);

/* Discovers the fan RPM/PWM sysfs paths once at startup (paths can differ
 * by hwmonN numbering across kernel versions/boards) and caches them in
 * static storage for read_snapshot() to use. Safe to call once; a repeat
 * call is a no-op. Missing fan hardware is not an error -- read_snapshot()
 * just reports fan_rpm/fan_percent as unavailable (-1). */
void sysinfo_find_fan_paths(void);

/* Fills `out` with a fresh snapshot. `cpu_reader` must be the same
 * instance across repeated calls (see above). Returns 0 on success ("ok"
 * fields best-effort -- individual missing sources just leave their
 * fields at the "-1"/unavailable sentinel rather than failing the whole
 * call), -1 only on a fatal setup problem (never expected in practice). */
int sysinfo_read_snapshot(ProcStatCpuReader *cpu_reader, SysSnapshot *out);

#endif /* JEU_SYSINFO_H */
