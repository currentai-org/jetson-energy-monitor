/*
 * sampler.h -- background thread sampling the INA3221 at target_hz,
 * mirroring src/sampler.py's Sampler class: preallocated ring buffer (no
 * per-sample heap allocation in the hot loop), clock_nanosleep absolute-
 * deadline pacing, and a rolling period-jitter stats window.
 */
#ifndef JEU_SAMPLER_H
#define JEU_SAMPLER_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

#include "common.h"
#include "ina3221.h"

#define SAMPLER_RING_CAPACITY 200000
#define SAMPLER_STATS_WINDOW 2000

typedef struct {
    long samples_taken;
    double target_hz;
    double achieved_hz;
    double mean_period_us;
    double max_period_us;
    double jitter_std_us;
} SamplerStats;

typedef struct {
    Ina3221 *dev;
    int channel;
    double target_period_s;

    /* Ring buffer: fixed-size, written by the sampler thread, read by
     * drain()/peek_recent() under `lock`. Mirrors sampler.py's parallel
     * array.array('d', ...) buffers. */
    double *buf_t;
    double *buf_ma;
    long capacity;
    volatile long write_count; /* monotonically increasing, never wraps */
    long read_count;           /* consumer-side: how many drained so far */
    pthread_mutex_t lock;

    /* Latest single reading, for cheap "peek without draining" access. */
    _Atomic double latest_t;
    _Atomic double latest_ma;
    atomic_int latest_valid;

    pthread_t thread;
    volatile sig_atomic_t stop_requested;
    int thread_running;

    /* Rolling period-jitter window (own lock-free-ish access is NOT
     * needed here since only stats() reads it and only the sampler
     * thread writes it; small risk of a torn read is acceptable for
     * a display statistic, matching the Python version's lack of extra
     * locking around this). */
    double *period_window;
    int period_window_len;
    int period_write_idx;
    int period_count;

    SamplerStats stats_snapshot; /* last_error omitted: samples_taken etc */
} Sampler;

/* Initializes a Sampler for `dev`/`channel` at `target_hz`. Does not start
 * the background thread yet (call sampler_start()). Returns 0 on success,
 * -1 on allocation failure. */
int sampler_init(Sampler *s, Ina3221 *dev, int channel, double target_hz);

/* Frees all buffers. Call sampler_stop() first if the thread was started. */
void sampler_free(Sampler *s);

/* Starts the background sampling thread. Returns 0 on success, -1 on
 * pthread_create failure. */
int sampler_start(Sampler *s);

/* Signals the sampling thread to stop and joins it (blocking). Safe to
 * call even if never started. */
void sampler_stop(Sampler *s);

/* Consumer-side: pops all samples since the last drain into `out`
 * (appended via sample_list_append/extend -- caller owns `out`'s
 * lifetime). Mirrors sampler.py's drain(). */
void sampler_drain(Sampler *s, SampleList *out);

/* Fills `stats` with the current SamplerStats snapshot (achieved rate,
 * jitter, etc, computed from the rolling period window). */
void sampler_get_stats(Sampler *s, SamplerStats *stats);

/* Peeks the single latest sample without draining the ring buffer.
 * Returns 1 if a sample has ever been taken (out filled), 0 otherwise. */
int sampler_peek_latest(Sampler *s, Sample *out);

#endif /* JEU_SAMPLER_H */
