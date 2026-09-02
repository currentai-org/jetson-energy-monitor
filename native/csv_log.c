/* csv_log.c -- see csv_log.h */
#include "csv_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void slugify(const char *name, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < out_size; i++) {
        char c = name[i];
        char lc = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        int keep = (lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9') || lc == '-' || lc == '_';
        out[j++] = keep ? lc : '_';
    }
    out[j] = '\0';
}

/* Builds "<cache_dir>/<timestamp>_<slug><suffix>.csv" into `out`. Returns
 * 0 on success, -1 if the cache dir couldn't be determined/created. */
static int build_csv_path(const char *test_name, const char *suffix, char *out, size_t out_size) {
    char *cache_dir = jeu_cache_dir();
    if (!cache_dir) return -1;

    char slug[128];
    slugify(test_name, slug, sizeof(slug));

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);

    snprintf(out, out_size, "%s/%s_%s%s.csv", cache_dir, ts, slug, suffix);
    free(cache_dir);
    return 0;
}

char *write_samples_csv(const char *test_name, const Sample *samples, size_t n,
                         double bus_voltage_v_or_negative) {
    if (n == 0) return NULL;

    char path[4096];
    if (build_csv_path(test_name, "", path, sizeof(path)) != 0) return NULL;

    FILE *f = fopen(path, "w");
    if (!f) return NULL;

    /* Large stdio buffer so this batched write doesn't get split into many
     * small write() syscalls (same rationale as ina_bench.c). */
    static char buf[1 << 20];
    setvbuf(f, buf, _IOFBF, sizeof(buf));

    int has_power = bus_voltage_v_or_negative >= 0.0;
    if (has_power) {
        fprintf(f, "sample_index,elapsed_s,current_ma,power_mw\n");
    } else {
        fprintf(f, "sample_index,elapsed_s,current_ma\n");
    }

    double t0 = samples[0].t;
    for (size_t i = 0; i < n; i++) {
        double elapsed = samples[i].t - t0;
        if (has_power) {
            fprintf(f, "%zu,%.6f,%.3f,%.3f\n", i, elapsed, samples[i].current_ma,
                    samples[i].current_ma * bus_voltage_v_or_negative);
        } else {
            fprintf(f, "%zu,%.6f,%.3f\n", i, elapsed, samples[i].current_ma);
        }
    }
    fclose(f);
    return strdup(path);
}

char *write_sysinfo_csv(const char *test_name, const SysSnapshot *sys_samples, size_t n) {
    if (n == 0) return NULL;

    char path[4096];
    if (build_csv_path(test_name, "_sysinfo", path, sizeof(path)) != 0) return NULL;

    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    static char buf[1 << 20];
    setvbuf(f, buf, _IOFBF, sizeof(buf));

    fprintf(f,
            "sample_index,elapsed_s,wall_time,cpu_percent,gpu_percent,ram_used_mb,"
            "ram_total_mb,ram_percent,swap_used_mb,swap_total_mb,swap_percent,fan_rpm,"
            "fan_percent,temp_c\n");

    double t0 = sys_samples[0].t;
    for (size_t i = 0; i < n; i++) {
        const SysSnapshot *s = &sys_samples[i];
        fprintf(f, "%zu,%.3f,%.3f,%.2f,%.2f,%.1f,%.1f,%.2f,%.1f,%.1f,%.2f,", i, s->t - t0, s->t,
                s->cpu_percent, s->gpu_percent, s->ram_used_mb, s->ram_total_mb, s->ram_percent,
                s->swap_used_mb, s->swap_total_mb, s->swap_percent);
        if (s->fan_rpm >= 0)
            fprintf(f, "%.1f,", s->fan_rpm);
        else
            fprintf(f, ",");
        if (s->fan_percent >= 0)
            fprintf(f, "%.1f,", s->fan_percent);
        else
            fprintf(f, ",");
        if (s->temp_c > -999.0)
            fprintf(f, "%.2f\n", s->temp_c);
        else
            fprintf(f, "\n");
    }
    fclose(f);
    return strdup(path);
}
