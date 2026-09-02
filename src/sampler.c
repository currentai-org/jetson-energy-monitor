/* sampler.c -- see sampler.h */
#include "sampler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLOCK_MONOTONIC_ID CLOCK_MONOTONIC
/* Absolute-deadline sleep threshold: same tuned value sampler.py arrived
 * at for clock_nanosleep-based pacing (see its module docstring). */
#define SPIN_THRESHOLD_S 0.00005

static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_until_absolute(double deadline) {
    struct timespec ts;
    ts.tv_sec = (time_t)deadline;
    ts.tv_nsec = (long)((deadline - ts.tv_sec) * 1e9);
    clock_nanosleep(CLOCK_MONOTONIC_ID, TIMER_ABSTIME, &ts, NULL);
}

int sampler_init(Sampler *s, Ina3221 *dev, int channel, double target_hz) {
    memset(s, 0, sizeof(*s));
    s->dev = dev;
    s->channel = channel;
    s->target_period_s = 1.0 / target_hz;
    s->capacity = SAMPLER_RING_CAPACITY;
    s->buf_t = calloc((size_t)s->capacity, sizeof(double));
    s->buf_ma = calloc((size_t)s->capacity, sizeof(double));
    s->period_window_len = SAMPLER_STATS_WINDOW;
    s->period_window = calloc((size_t)s->period_window_len, sizeof(double));
    if (!s->buf_t || !s->buf_ma || !s->period_window) {
        sampler_free(s);
        return -1;
    }
    pthread_mutex_init(&s->lock, NULL);
    atomic_init(&s->latest_valid, 0);
    s->stats_snapshot.target_hz = target_hz;
    return 0;
}

void sampler_free(Sampler *s) {
    free(s->buf_t);
    free(s->buf_ma);
    free(s->period_window);
    s->buf_t = NULL;
    s->buf_ma = NULL;
    s->period_window = NULL;
    pthread_mutex_destroy(&s->lock);
}

static void *sampler_thread_main(void *arg) {
    Sampler *s = (Sampler *)arg;
    double target_period = s->target_period_s;
    long capacity = s->capacity;

    double next_deadline = now_monotonic();
    double last_t = next_deadline;
    long n = 0;

    while (!s->stop_requested) {
        double now = now_monotonic();
        if (now < next_deadline) {
            double remaining = next_deadline - now;
            if (remaining > SPIN_THRESHOLD_S) {
                sleep_until_absolute(next_deadline - SPIN_THRESHOLD_S);
            }
            continue;
        }

        double current_ma;
        if (ina3221_read_current_ma(s->dev, s->channel, &current_ma) != 0) {
            /* Transient I2C hiccup: skip this sample, keep schedule. */
            next_deadline += target_period;
            continue;
        }
        double t = now_monotonic();

        long idx = s->write_count % capacity;
        pthread_mutex_lock(&s->lock);
        s->buf_t[idx] = t;
        s->buf_ma[idx] = current_ma;
        s->write_count++;
        pthread_mutex_unlock(&s->lock);

        atomic_store(&s->latest_t, t);
        atomic_store(&s->latest_ma, current_ma);
        atomic_store(&s->latest_valid, 1);

        s->period_window[s->period_write_idx] = t - last_t;
        s->period_write_idx = (s->period_write_idx + 1) % s->period_window_len;
        if (s->period_count < s->period_window_len) s->period_count++;
        last_t = t;
        n++;

        next_deadline += target_period;
        if (now - next_deadline > target_period * 2) {
            next_deadline = now + target_period;
        }
    }

    s->stats_snapshot.samples_taken = n;
    return NULL;
}

int sampler_start(Sampler *s) {
    s->stop_requested = 0;
    int rc = pthread_create(&s->thread, NULL, sampler_thread_main, s);
    if (rc != 0) return -1;
    s->thread_running = 1;
    return 0;
}

void sampler_stop(Sampler *s) {
    if (!s->thread_running) return;
    s->stop_requested = 1;
    pthread_join(s->thread, NULL);
    s->thread_running = 0;
}

void sampler_drain(Sampler *s, SampleList *out) {
    pthread_mutex_lock(&s->lock);
    long start = s->read_count;
    long end = s->write_count;
    if (end - start > s->capacity) {
        start = end - s->capacity;
    }
    s->read_count = end;
    long capacity = s->capacity;
    long count = end - start;
    if (count > 0) {
        /* Fast path when the requested range doesn't wrap the ring
         * buffer: one memcpy-able extend via a temporary stack/heap
         * scratch, built as Sample structs. */
        Sample *tmp = malloc((size_t)count * sizeof(Sample));
        if (tmp) {
            for (long i = 0; i < count; i++) {
                long idx = (start + i) % capacity;
                tmp[i].t = s->buf_t[idx];
                tmp[i].current_ma = s->buf_ma[idx];
            }
            sample_list_extend(out, tmp, (size_t)count);
            free(tmp);
        }
    }
    pthread_mutex_unlock(&s->lock);
}

void sampler_get_stats(Sampler *s, SamplerStats *stats) {
    *stats = s->stats_snapshot;
    int count = s->period_count;
    if (count > 0) {
        double *window = s->period_window;
        int len = s->period_window_len;
        double sum = 0.0, maxp = 0.0;
        int valid_count = (count < len) ? count : len;
        for (int i = 0; i < valid_count; i++) {
            sum += window[i];
            if (window[i] > maxp) maxp = window[i];
        }
        double mean_p = sum / valid_count;
        stats->achieved_hz = mean_p > 0 ? 1.0 / mean_p : 0.0;
        stats->mean_period_us = mean_p * 1e6;
        stats->max_period_us = maxp * 1e6;
        double var = 0.0;
        for (int i = 0; i < valid_count; i++) {
            double d = window[i] - mean_p;
            var += d * d;
        }
        var /= valid_count;
        stats->jitter_std_us = sqrt(var) * 1e6;
    }
}

int sampler_peek_latest(Sampler *s, Sample *out) {
    if (!atomic_load(&s->latest_valid)) return 0;
    out->t = atomic_load(&s->latest_t);
    out->current_ma = atomic_load(&s->latest_ma);
    return 1;
}
