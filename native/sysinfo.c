/* sysinfo.c -- see sysinfo.h */
#include "sysinfo.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GPU_LOAD_PATH "/sys/devices/platform/bus@0/17000000.gpu/load"
#define MEMINFO_PATH "/proc/meminfo"
#define PROC_STAT_PATH "/proc/stat"
#define THERMAL_DIR "/sys/class/thermal"
#define HWMON_DIR "/sys/class/hwmon"

static char g_fan_rpm_path[320] = {0};
static char g_fan_pwm_path[320] = {0};
static int g_fan_paths_discovered = 0;

static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void proc_stat_cpu_reader_init(ProcStatCpuReader *r) {
    memset(r, 0, sizeof(*r));
    r->has_prev = 0;
}

/* Reads the aggregate "cpu " line's 7 fields (user/nice/system/idle/
 * iowait/irq/softirq) and returns the utilization percent as the delta
 * since the previous call (0.0 on the first call, matching sampler.py's
 * _ProcStatCpuReader). */
static double read_cpu_percent(ProcStatCpuReader *r) {
    FILE *f = fopen(PROC_STAT_PATH, "r");
    if (!f) return 0.0;
    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0.0;
    }
    fclose(f);

    long long fields[8] = {0};
    /* "cpu  user nice system idle iowait irq softirq ..." */
    sscanf(line, "cpu %lld %lld %lld %lld %lld %lld %lld", &fields[0], &fields[1], &fields[2],
           &fields[3], &fields[4], &fields[5], &fields[6]);

    if (!r->has_prev) {
        memcpy(r->prev_total, fields, sizeof(fields));
        r->has_prev = 1;
        return 0.0;
    }

    long long delta[7];
    long long total = 0;
    for (int i = 0; i < 7; i++) {
        delta[i] = fields[i] - r->prev_total[i];
        total += delta[i];
    }
    memcpy(r->prev_total, fields, sizeof(fields));

    if (total <= 0) return 0.0;
    long long idle = delta[3]; /* index 3 = idle */
    return 100.0 * (1.0 - (double)idle / (double)total);
}

static double read_gpu_percent(void) {
    FILE *f = fopen(GPU_LOAD_PATH, "r");
    if (!f) return 0.0;
    int raw = 0;
    int ok = fscanf(f, "%d", &raw) == 1;
    fclose(f);
    /* jtop/core/gpu.py parses this same file as `load / 10.0` -> percent
     * (0-1000 raw scale). */
    return ok ? raw / 10.0 : 0.0;
}

static void read_mem_info(double *ram_used_mb, double *ram_total_mb, double *ram_percent,
                           double *swap_used_mb, double *swap_total_mb, double *swap_percent) {
    *ram_used_mb = *ram_total_mb = *ram_percent = 0.0;
    *swap_used_mb = *swap_total_mb = *swap_percent = 0.0;

    FILE *f = fopen(MEMINFO_PATH, "r");
    if (!f) return;
    long long mem_total_kb = 0, mem_available_kb = 0;
    long long swap_total_kb = 0, swap_free_kb = 0;
    char key[64];
    long long value;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63[^:]: %lld", key, &value) == 2) {
            if (strcmp(key, "MemTotal") == 0) mem_total_kb = value;
            else if (strcmp(key, "MemAvailable") == 0) mem_available_kb = value;
            else if (strcmp(key, "SwapTotal") == 0) swap_total_kb = value;
            else if (strcmp(key, "SwapFree") == 0) swap_free_kb = value;
        }
    }
    fclose(f);

    *ram_total_mb = mem_total_kb / 1024.0;
    double mem_used_kb = mem_total_kb - mem_available_kb;
    *ram_used_mb = mem_used_kb / 1024.0;
    *ram_percent = (*ram_total_mb > 0) ? (*ram_used_mb / *ram_total_mb * 100.0) : 0.0;

    *swap_total_mb = swap_total_kb / 1024.0;
    double swap_used_kb = swap_total_kb - swap_free_kb;
    *swap_used_mb = swap_used_kb / 1024.0;
    *swap_percent = (*swap_total_mb > 0) ? (*swap_used_mb / *swap_total_mb * 100.0) : 0.0;
}

/* Reads one thermal zone's millidegree temp file -> Celsius, or returns
 * 0 (caller should check the zone's `type` was matched before trusting
 * this) if unreadable. */
static int read_thermal_zone_temp_c(const char *zone_dir, double *out_c) {
    char path[320];
    snprintf(path, sizeof(path), "%s/temp", zone_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long millideg = 0;
    int ok = fscanf(f, "%ld", &millideg) == 1;
    fclose(f);
    if (!ok) return 0;
    *out_c = millideg / 1000.0;
    return 1;
}

/* Scans /sys/class/thermal/thermal_zone* for the "tj" zone (thermal
 * junction, preferred -- matches sysinfo.py's priority) or, failing that,
 * the hottest zone among all readable zones. Returns 1 and fills out_c on
 * success, 0 if no zones were readable at all. */
static int read_die_temp_c(double *out_c) {
    DIR *d = opendir(THERMAL_DIR);
    if (!d) return 0;

    double tj_temp = 0.0;
    int tj_found = 0;
    double hottest = -1000.0;
    int any_found = 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
        char zone_dir[300];
        snprintf(zone_dir, sizeof(zone_dir), "%s/%s", THERMAL_DIR, entry->d_name);
        char type_path[320];
        snprintf(type_path, sizeof(type_path), "%s/type", zone_dir);
        FILE *tf = fopen(type_path, "r");
        if (!tf) continue;
        char type_buf[64] = {0};
        if (!fgets(type_buf, sizeof(type_buf), tf)) {
            fclose(tf);
            continue;
        }
        fclose(tf);
        /* strip trailing newline */
        size_t tl = strlen(type_buf);
        if (tl > 0 && type_buf[tl - 1] == '\n') type_buf[tl - 1] = '\0';

        double temp_c;
        if (!read_thermal_zone_temp_c(zone_dir, &temp_c)) continue;
        /* Sanity filter matching sysinfo.py's z.get('temp', -256) > -100
         * check -- ignore obviously-invalid sensor readings. */
        if (temp_c <= -100.0) continue;

        any_found = 1;
        if (temp_c > hottest) hottest = temp_c;
        if (strcmp(type_buf, "tj") == 0) {
            tj_temp = temp_c;
            tj_found = 1;
        }
    }
    closedir(d);

    if (tj_found) {
        *out_c = tj_temp;
        return 1;
    }
    if (any_found) {
        *out_c = hottest;
        return 1;
    }
    return 0;
}

void sysinfo_find_fan_paths(void) {
    if (g_fan_paths_discovered) return;
    g_fan_paths_discovered = 1;

    /* PWM duty cycle: known stable path on Jetson Orin Nano dev kits. */
    snprintf(g_fan_pwm_path, sizeof(g_fan_pwm_path),
             "/sys/devices/platform/pwm-fan/hwmon/hwmon0/pwm1");
    FILE *test = fopen(g_fan_pwm_path, "r");
    if (test) {
        fclose(test);
    } else {
        g_fan_pwm_path[0] = '\0';
    }

    /* Fan tachometer RPM: hwmonN numbering can vary by kernel/board, so
     * scan /sys/class/hwmon/hwmon* for one that exposes an "rpm" file
     * (only the fan tachometer driver does, on this platform). */
    DIR *d = opendir(HWMON_DIR);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;
            char candidate[300];
            snprintf(candidate, sizeof(candidate), "%s/%s/rpm", HWMON_DIR, entry->d_name);
            FILE *f = fopen(candidate, "r");
            if (f) {
                fclose(f);
                snprintf(g_fan_rpm_path, sizeof(g_fan_rpm_path), "%s", candidate);
                break;
            }
        }
        closedir(d);
    }
}

int sysinfo_read_snapshot(ProcStatCpuReader *cpu_reader, SysSnapshot *out) {
    memset(out, 0, sizeof(*out));
    out->t = now_monotonic();
    out->cpu_percent = read_cpu_percent(cpu_reader);
    out->gpu_percent = read_gpu_percent();
    read_mem_info(&out->ram_used_mb, &out->ram_total_mb, &out->ram_percent, &out->swap_used_mb,
                  &out->swap_total_mb, &out->swap_percent);

    out->fan_rpm = -1.0;
    out->fan_percent = -1.0;
    if (g_fan_rpm_path[0]) {
        FILE *f = fopen(g_fan_rpm_path, "r");
        if (f) {
            int rpm = 0;
            if (fscanf(f, "%d", &rpm) == 1) out->fan_rpm = rpm;
            fclose(f);
        }
    }
    if (g_fan_pwm_path[0]) {
        FILE *f = fopen(g_fan_pwm_path, "r");
        if (f) {
            int pwm = 0;
            if (fscanf(f, "%d", &pwm) == 1) out->fan_percent = pwm * 100.0 / 255.0;
            fclose(f);
        }
    }

    double temp_c;
    out->temp_c = read_die_temp_c(&temp_c) ? temp_c : -1000.0;

    out->ok = 1;
    return 0;
}
