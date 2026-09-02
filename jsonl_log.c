/* jsonl_log.c -- see jsonl_log.h. Hand-rolled minimal JSON writer (no
 * external JSON library dependency) -- fields are all known simple types
 * (numbers, strings, null, nested "object" only one level deep for
 * sysinfo summary fields, which we flatten instead -- see below), so a
 * small dedicated writer is simpler and lighter than pulling in a general
 * JSON library for this. */
#include "jsonl_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Escapes a string for JSON (handles quotes/backslashes/control chars --
 * sufficient for our controlled inputs: hostnames, paths, names). */
static void json_escape_and_write(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20) {
                    fprintf(f, "\\u%04x", *p);
                } else {
                    fputc(*p, f);
                }
        }
    }
    fputc('"', f);
}

static void json_field_str(FILE *f, const char *key, const char *value, int *first) {
    if (!*first) fputc(',', f);
    *first = 0;
    json_escape_and_write(f, key);
    fputc(':', f);
    if (value) {
        json_escape_and_write(f, value);
    } else {
        fputs("null", f);
    }
}

static void json_field_num(FILE *f, const char *key, double value, int *first) {
    if (!*first) fputc(',', f);
    *first = 0;
    json_escape_and_write(f, key);
    fprintf(f, ":%.6g", value);
}

static void json_field_int(FILE *f, const char *key, long value, int *first) {
    if (!*first) fputc(',', f);
    *first = 0;
    json_escape_and_write(f, key);
    fprintf(f, ":%ld", value);
}

static void json_field_null(FILE *f, const char *key, int *first) {
    if (!*first) fputc(',', f);
    *first = 0;
    json_escape_and_write(f, key);
    fputs(":null", f);
}

char *append_result_jsonl(const JsonlRecordInput *in) {
    char *cache_dir = jeu_cache_dir();
    if (!cache_dir) return NULL;

    char path[4096];
    snprintf(path, sizeof(path), "%s/results.jsonl", cache_dir);
    free(cache_dir);

    FILE *f = fopen(path, "a");
    if (!f) return NULL;

    time_t now_wall = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now_wall, &tm_utc);
    char iso[32];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    char hostname[256] = "unknown";
    gethostname(hostname, sizeof(hostname));

    char addr_hex[16];
    snprintf(addr_hex, sizeof(addr_hex), "0x%x", in->i2c_address);

    int first = 1;
    fputc('{', f);

    json_field_str(f, "test_type", in->test_type, &first);
    json_field_num(f, "logged_at", (double)now_wall, &first);
    json_field_str(f, "logged_at_iso", iso, &first);
    json_field_str(f, "hostname", hostname, &first);

    json_field_str(f, "name", in->name, &first);
    json_field_num(f, "duration_s", in->duration_s, &first);
    json_field_int(f, "n_samples", in->n_samples, &first);
    json_field_num(f, "mean_current_ma", in->mean_current_ma, &first);
    json_field_num(f, "min_current_ma", in->min_current_ma, &first);
    json_field_num(f, "max_current_ma", in->max_current_ma, &first);
    json_field_num(f, "achieved_hz", in->achieved_hz, &first);

    if (in->has_bus_voltage)
        json_field_num(f, "bus_voltage_v", in->bus_voltage_v, &first);
    else
        json_field_null(f, "bus_voltage_v", &first);

    if (in->has_mean_power)
        json_field_num(f, "mean_power_mw", in->mean_power_mw, &first);
    else
        json_field_null(f, "mean_power_mw", &first);

    if (in->has_energy) {
        json_field_num(f, "mwh", in->mwh, &first);
        json_field_num(f, "mah", in->mah, &first);
    } else {
        json_field_null(f, "mwh", &first);
        json_field_null(f, "mah", &first);
    }

    json_field_str(f, "csv_path", in->csv_path, &first);

    json_field_num(f, "sampler_target_hz", in->sampler_target_hz, &first);
    json_field_num(f, "sampler_mean_period_us", in->sampler_mean_period_us, &first);
    json_field_num(f, "sampler_max_period_us", in->sampler_max_period_us, &first);
    json_field_num(f, "sampler_jitter_std_us", in->sampler_jitter_std_us, &first);
    json_field_null(f, "sampler_last_error", &first); /* native impl doesn't track this yet */

    json_field_int(f, "i2c_channel", in->i2c_channel, &first);
    json_field_str(f, "i2c_channel_name", in->i2c_channel_name, &first);
    json_field_num(f, "shunt_ohms", in->shunt_ohms, &first);
    json_field_str(f, "i2c_address", addr_hex, &first);

    json_field_str(f, "sysinfo_csv_path", in->sysinfo_csv_path, &first);

    if (in->has_sysinfo_summary) {
        const SysSummaryStats *s = &in->sysinfo_summary;
        json_field_int(f, "sys_n_samples", s->sys_n_samples, &first);
        json_field_num(f, "sys_avg_cpu_percent", s->avg_cpu_percent, &first);
        json_field_num(f, "sys_min_cpu_percent", s->min_cpu_percent, &first);
        json_field_num(f, "sys_max_cpu_percent", s->max_cpu_percent, &first);
        json_field_num(f, "sys_avg_gpu_percent", s->avg_gpu_percent, &first);
        json_field_num(f, "sys_min_gpu_percent", s->min_gpu_percent, &first);
        json_field_num(f, "sys_max_gpu_percent", s->max_gpu_percent, &first);
        json_field_num(f, "sys_avg_ram_percent", s->avg_ram_percent, &first);
        json_field_num(f, "sys_min_ram_percent", s->min_ram_percent, &first);
        json_field_num(f, "sys_max_ram_percent", s->max_ram_percent, &first);
        json_field_num(f, "sys_avg_swap_percent", s->avg_swap_percent, &first);
        json_field_num(f, "sys_min_swap_percent", s->min_swap_percent, &first);
        json_field_num(f, "sys_max_swap_percent", s->max_swap_percent, &first);
        if (s->has_fan_rpm) {
            json_field_num(f, "sys_avg_fan_rpm", s->avg_fan_rpm, &first);
            json_field_num(f, "sys_min_fan_rpm", s->min_fan_rpm, &first);
            json_field_num(f, "sys_max_fan_rpm", s->max_fan_rpm, &first);
        } else {
            json_field_null(f, "sys_avg_fan_rpm", &first);
            json_field_null(f, "sys_min_fan_rpm", &first);
            json_field_null(f, "sys_max_fan_rpm", &first);
        }
        if (s->has_temp) {
            json_field_num(f, "sys_avg_temp_c", s->avg_temp_c, &first);
            json_field_num(f, "sys_min_temp_c", s->min_temp_c, &first);
            json_field_num(f, "sys_max_temp_c", s->max_temp_c, &first);
        } else {
            json_field_null(f, "sys_avg_temp_c", &first);
            json_field_null(f, "sys_min_temp_c", &first);
            json_field_null(f, "sys_max_temp_c", &first);
        }
    }

    if (in->requested_duration_s >= 0.0) {
        json_field_num(f, "requested_duration_s", in->requested_duration_s, &first);
    }

    fputs("}\n", f);
    fclose(f);
    return strdup(path);
}
