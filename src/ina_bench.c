/*
 * ina_bench.c -- minimal-overhead INA3221 current-sampling benchmark, pure C.
 *
 * Purpose: a performance REFERENCE POINT for the Python sampler (see
 * ../src/sampler.py) on the native-sampler-spike branch. Not wired into the
 * app; a standalone benchmark to answer "what's the floor for CPU%/jitter
 * if we strip away Python/GIL/Textual entirely and just read+queue+flush?"
 * before deciding whether a Cython/C extension is worth the complexity.
 *
 * Scope (deliberately minimal, per the spike's stated goal):
 *   - Raw I2C ioctl register access to the INA3221 (bus 1, addr 0x40),
 *     mirroring ina3221.py's register map and "fast single channel" config
 *     (VBUSCT=VSHCT=140us, AVG=1, continuous shunt+bus mode) -- no libi2c/
 *     smbus wrapper, just linux/i2c-dev.h + linux/i2c.h ioctl(I2C_RDWR).
 *   - Absolute-deadline pacing via clock_nanosleep(CLOCK_MONOTONIC,
 *     TIMER_ABSTIME, ...) -- same technique sampler.py already uses via
 *     ctypes, now natively.
 *   - Samples queued into a preallocated ring buffer (struct { double t;
 *     double current_ma; }), no per-sample malloc.
 *   - Periodic CSV flush: once FLUSH_BATCH samples have accrued, one
 *     buffered fwrite() of the whole batch as a single formatted text
 *     blob -- not one fprintf() per sample -- to keep write-side syscall/
 *     libc overhead off the hot sampling path. Uses a large stdio buffer
 *     (setvbuf) on top of that.
 *   - SIGINT (Ctrl-C) stops cleanly: flushes any partial batch, restores
 *     the INA3221's original config register, closes the fd, prints a
 *     summary (samples, elapsed, achieved Hz, jitter stddev, max period),
 *     same fields sampler.py's SamplerStats reports so the two are
 *     directly comparable.
 *
 * Build:
 *   gcc -O2 -Wall -o ina_bench ina_bench.c
 *
 * Run (no sudo needed if your user is in the i2c group, same as the
 * Python driver):
 *   ./ina_bench [--hz N] [--duration SEC] [--channel 1|2|3] [--out PATH]
 *
 * Default: 1000 Hz, runs until Ctrl-C or --duration elapses, channel 1
 * (VDD_IN), writes to ./ina_bench_samples.csv.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define I2C_BUS_PATH "/dev/i2c-1"
#define INA3221_ADDR 0x40
#define REG_CONFIG 0x00
#define SHUNT_LSB_UV 40.0     /* microvolts per LSB, 13-bit signed shunt reg */
#define SHUNT_OHMS 0.005      /* board shunt resistor */

/* Ring buffer / batching */
#define RING_CAPACITY 200000  /* mirrors sampler.py's buffer_maxlen */
#define FLUSH_BATCH 2000      /* flush to CSV once this many samples accrue */

/* Stats window for jitter/period tracking (mirrors sampler.py) */
#define STATS_WINDOW 2000

typedef struct {
    double t;           /* CLOCK_MONOTONIC seconds */
    double current_ma;
} Sample;

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signum) {
    (void)signum;
    g_stop = 1;
}

/* --- I2C raw register access (write-then-read combined transaction, same
   shape as smbus2.i2c_rdwr: a repeated START, not stop-then-start) -------- */

static int i2c_read_word(int fd, uint8_t reg, uint16_t *out) {
    uint8_t reg_buf[1] = {reg};
    uint8_t data_buf[2] = {0, 0};

    struct i2c_msg msgs[2] = {
        {.addr = INA3221_ADDR, .flags = 0, .len = 1, .buf = reg_buf},
        {.addr = INA3221_ADDR, .flags = I2C_M_RD, .len = 2, .buf = data_buf},
    };
    struct i2c_rdwr_ioctl_data rdwr = {.msgs = msgs, .nmsgs = 2};

    if (ioctl(fd, I2C_RDWR, &rdwr) < 0) {
        return -1;
    }
    *out = ((uint16_t)data_buf[0] << 8) | data_buf[1];
    return 0;
}

static int i2c_write_word(int fd, uint8_t reg, uint16_t value) {
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    struct i2c_msg msg = {.addr = INA3221_ADDR, .flags = 0, .len = 3, .buf = buf};
    struct i2c_rdwr_ioctl_data rdwr = {.msgs = &msg, .nmsgs = 1};
    return ioctl(fd, I2C_RDWR, &rdwr) < 0 ? -1 : 0;
}

/* Config word layout, per TI INA3221 datasheet (SBOS689) -- mirrors
   ina3221.py's Ina3221Config.to_word(). VBUSCT/VSHCT=0b000 (140us),
   AVG=0b000 (1 sample), MODE=0b111 (continuous shunt+bus). */
static uint16_t make_config_word(int channel) {
    uint16_t word = 0;
    if (channel == 1) word |= (1 << 14);
    if (channel == 2) word |= (1 << 13);
    if (channel == 3) word |= (1 << 12);
    /* vbusct = 0, vshct = 0 -> both fields already zero */
    /* avg = 0 -> already zero */
    word |= 0b111; /* mode: continuous shunt + bus */
    return word;
}

static double shunt_word_to_ma(uint16_t raw) {
    int16_t signed_raw = (int16_t)raw;
    int32_t signed_val = ((int32_t)signed_raw) >> 3; /* 13-bit, left-justified */
    double uv = signed_val * SHUNT_LSB_UV;
    double amps = (uv * 1e-6) / SHUNT_OHMS;
    return amps * 1000.0;
}

/* --- CSV flush: batch-format then a single fwrite, not one fprintf/sample */

static char *g_flush_text_buf = NULL;
static size_t g_flush_text_cap = 0;

static void flush_batch_to_csv(FILE *out, const Sample *buf, int count, long *sample_index) {
    if (count <= 0) return;
    /* Conservative per-line estimate: "index,elapsed_s,current_ma\n" */
    size_t need = (size_t)count * 40 + 64;
    if (need > g_flush_text_cap) {
        free(g_flush_text_buf);
        g_flush_text_buf = malloc(need);
        g_flush_text_cap = need;
    }
    char *p = g_flush_text_buf;
    for (int i = 0; i < count; i++) {
        p += sprintf(p, "%ld,%.6f,%.3f\n", (*sample_index)++, buf[i].t, buf[i].current_ma);
    }
    fwrite(g_flush_text_buf, 1, (size_t)(p - g_flush_text_buf), out);
}

int main(int argc, char **argv) {
    double target_hz = 1000.0;
    double duration_s = -1.0; /* -1 = run until SIGINT */
    int channel = 1;
    const char *out_path = "ina_bench_samples.csv";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc) {
            target_hz = atof(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            channel = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--hz N] [--duration SEC] [--channel 1|2|3] [--out PATH]\n", argv[0]);
            return 0;
        }
    }
    if (channel < 1 || channel > 3) {
        fprintf(stderr, "channel must be 1, 2, or 3\n");
        return 1;
    }

    signal(SIGINT, on_sigint);

    int fd = open(I2C_BUS_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", I2C_BUS_PATH, strerror(errno));
        return 1;
    }
    /* Deliberately no ioctl(fd, I2C_SLAVE, ...) here: the kernel's hwmon
       driver is already bound to this address (see ina3221.py's docstring
       -- the stock hwmon config multiplexes all three channels at a slow
       ~77Hz aggregate rate), and I2C_SLAVE would fail with EBUSY since it
       claims exclusive ownership of that address on the bus. I2C_RDWR
       (used throughout below, mirroring smbus2.i2c_rdwr in the Python
       driver) carries the target address in each i2c_msg instead, which
       coexists fine with the kernel driver already using the same address
       for its own periodic reads. */

    uint16_t orig_config;
    if (i2c_read_word(fd, REG_CONFIG, &orig_config) < 0) {
        fprintf(stderr, "read original config: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    uint16_t fast_config = make_config_word(channel);
    if (i2c_write_word(fd, REG_CONFIG, fast_config) < 0) {
        fprintf(stderr, "write fast config: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint8_t shunt_reg = (channel == 1) ? 0x01 : (channel == 2) ? 0x03 : 0x05;

    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "fopen(%s): %s\n", out_path, strerror(errno));
        i2c_write_word(fd, REG_CONFIG, orig_config);
        close(fd);
        return 1;
    }
    /* Large stdio buffer so our own batched fwrite()s don't get split into
       many small write() syscalls underneath us. */
    static char stdio_buf[1 << 20];
    setvbuf(out, stdio_buf, _IOFBF, sizeof(stdio_buf));
    fprintf(out, "sample_index,elapsed_s,current_ma\n");

    Sample *ring = malloc(sizeof(Sample) * FLUSH_BATCH);
    int ring_count = 0;
    long sample_index = 0;

    double target_period = 1.0 / target_hz;
    double period_window[STATS_WINDOW];
    int period_write_idx = 0;
    int period_count = 0;

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    double t_start = ts_start.tv_sec + ts_start.tv_nsec * 1e-9;
    double t0_for_csv = t_start;

    double next_deadline = t_start;
    double last_t = t_start;
    long n = 0;
    const double SPIN_THRESHOLD_S = 0.00005; /* matches sampler.py's tuned value */

    while (!g_stop) {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        double now = ts_now.tv_sec + ts_now.tv_nsec * 1e-9;

        if (duration_s >= 0.0 && now - t_start >= duration_s) {
            break;
        }

        if (now < next_deadline) {
            double remaining = next_deadline - now;
            if (remaining > SPIN_THRESHOLD_S) {
                double target = next_deadline - SPIN_THRESHOLD_S;
                struct timespec ts_target;
                ts_target.tv_sec = (time_t)target;
                ts_target.tv_nsec = (long)((target - ts_target.tv_sec) * 1e9);
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts_target, NULL);
            }
            continue;
        }

        uint16_t raw;
        if (i2c_read_word(fd, shunt_reg, &raw) < 0) {
            /* Transient I2C hiccup: skip this sample, keep the loop's
               deadline schedule intact rather than aborting. */
            next_deadline += target_period;
            continue;
        }
        double current_ma = shunt_word_to_ma(raw);

        struct timespec ts_read;
        clock_gettime(CLOCK_MONOTONIC, &ts_read);
        double t = ts_read.tv_sec + ts_read.tv_nsec * 1e-9;

        ring[ring_count].t = t - t0_for_csv;
        ring[ring_count].current_ma = current_ma;
        ring_count++;
        if (ring_count >= FLUSH_BATCH) {
            flush_batch_to_csv(out, ring, ring_count, &sample_index);
            ring_count = 0;
        }

        period_window[period_write_idx] = t - last_t;
        period_write_idx = (period_write_idx + 1) % STATS_WINDOW;
        if (period_count < STATS_WINDOW) period_count++;
        last_t = t;
        n++;

        next_deadline += target_period;
        if (now - next_deadline > target_period * 2) {
            next_deadline = now + target_period;
        }
    }

    /* Flush any partial final batch */
    flush_batch_to_csv(out, ring, ring_count, &sample_index);
    fclose(out);
    free(ring);
    free(g_flush_text_buf);

    /* Restore original config, close bus */
    i2c_write_word(fd, REG_CONFIG, orig_config);
    close(fd);

    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double t_end = ts_end.tv_sec + ts_end.tv_nsec * 1e-9;
    double elapsed = t_end - t_start;

    double mean_period = 0.0, jitter_std = 0.0, max_period = 0.0;
    if (period_count > 0) {
        int count = period_count;
        double sum = 0.0;
        double maxp = 0.0;
        for (int i = 0; i < count; i++) {
            sum += period_window[i];
            if (period_window[i] > maxp) maxp = period_window[i];
        }
        mean_period = sum / count;
        double var = 0.0;
        for (int i = 0; i < count; i++) {
            double d = period_window[i] - mean_period;
            var += d * d;
        }
        var /= count;
        jitter_std = sqrt(var);
        max_period = maxp;
    }
    double achieved_hz = mean_period > 0.0 ? 1.0 / mean_period : 0.0;

    printf("\nina_bench summary\n");
    printf("  channel:        %d\n", channel);
    printf("  target rate:    %.1f Hz\n", target_hz);
    printf("  elapsed:        %.3f s\n", elapsed);
    printf("  samples:        %ld\n", n);
    printf("  achieved rate:  %.1f Hz\n", achieved_hz);
    printf("  mean period:    %.1f us\n", mean_period * 1e6);
    printf("  jitter (std):   %.1f us\n", jitter_std * 1e6);
    printf("  max period:     %.1f us\n", max_period * 1e6);
    printf("  output CSV:     %s\n", out_path);

    return 0;
}
