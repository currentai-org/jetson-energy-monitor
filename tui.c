/* tui.c -- see tui.h */
#include "tui.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "screen.h"
#include "sparkline.h"
#include "term.h"
#include "tests.h"

#define UI_REFRESH_HZ 5.0    /* deliberately modest -- this is a "rough
                                 picture" tool, not an oscilloscope; a
                                 higher rate mostly burns CPU on redraws */
#define VOLTAGE_POLL_HZ 1.0  /* throttled bus-voltage read, same rationale
                                 as sampler.py's dev_last_voltage() */
#define LOG_MAX_LINES 200
/* Fixed rows above/below the log region: Mode/Channel line + CPU/GPU/RAM
 * line + 4 sparklines * SPARKLINE_ROWS rows each + 1 blank separator + 1
 * footer. Kept as a named constant so compute_log_visible_lines() and
 * the fixed-row layout in render() can't silently drift apart. */
#define SPARKLINE_ROWS 2 /* stacked rows per sparkline -- see sparkline.h's
                             sparkline_render_rows(): doubles the
                             effective vertical resolution to 16 levels
                             (2*8) versus a single 8-level row, at zero
                             extra sampling/CPU cost (same data, just
                             split across two lines of text). */
#define FIXED_CHROME_ROWS (2 + 4 * SPARKLINE_ROWS + 1 + 1)
#define LOG_VISIBLE_LINES_MIN 3  /* always show at least this many, even on
                                    a very short terminal */
#define LOG_VISIBLE_LINES_DEFAULT 13 /* used before the first term_get_size()
                                        call, and as a floor/typical value */
/* Hard cap: must leave room for FIXED_CHROME_ROWS within SCREEN_MAX_LINES
 * (screen.h), and bounds the on-stack `recent[]` scratch array in
 * render(). */
#define LOG_VISIBLE_LINES_MAX (SCREEN_MAX_LINES - FIXED_CHROME_ROWS - 2)

static volatile sig_atomic_t g_tui_stop = 0;

static void on_sigint_tui(int signum) {
    (void)signum;
    g_tui_stop = 1;
}

/* --- Small ring-buffer log, mirroring the old Textual Log widget's
 * "append a line, keep the most recent N visible" behavior. --- */
typedef struct {
    char lines[LOG_MAX_LINES][256];
    int count;     /* total ever written (monotonic) */
    int write_idx; /* next slot to write */
} TuiLog;

static void tui_log_init(TuiLog *log) {
    memset(log, 0, sizeof(*log));
}

static void tui_log_write(TuiLog *log, const char *fmt, ...) {
    char *slot = log->lines[log->write_idx];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot, sizeof(log->lines[0]), fmt, ap);
    va_end(ap);
    log->write_idx = (log->write_idx + 1) % LOG_MAX_LINES;
    log->count++;
}

/* Fills `out[0..n)` with the n most recent log lines in chronological
 * order (oldest first); returns how many were actually available
 * (<= n). */
static int tui_log_recent(const TuiLog *log, const char **out, int n) {
    int available = (log->count < LOG_MAX_LINES) ? log->count : LOG_MAX_LINES;
    int take = (n < available) ? n : available;
    int start = (log->write_idx - take + LOG_MAX_LINES) % LOG_MAX_LINES;
    for (int i = 0; i < take; i++) {
        out[i] = log->lines[(start + i) % LOG_MAX_LINES];
    }
    return take;
}

/* --- Application state (mirrors the old Python EnergyApp's fields) --- */
typedef struct {
    Ina3221 *dev;
    Sampler *sampler;
    SysMonitor *sysmon;
    int channel;
    int use_color; /* --color/--no-color: enable truecolor sparkline
                       gradients (see sparkline.h) */

    SparklineHistory hist_current;
    SparklineHistory hist_cpu;
    SparklineHistory hist_gpu;
    SparklineHistory hist_temp;

    char mode[64]; /* "idle", "BASELINE RUNNING (10s)...", etc */

    /* Baseline test runs in its own thread (like the Python worker
     * thread) so the render loop never blocks on it. */
    pthread_t baseline_thread;
    int baseline_running;
    int baseline_thread_joinable;
    TestResult baseline_result;
    volatile sig_atomic_t baseline_done_flag; /* set by the worker thread */

    EnergyCapture energy;
    int energy_armed;

    /* Comment prompt (triggered by 'e'): while prompting_comment is set,
     * keystrokes build up comment_input instead of being dispatched as
     * normal hotkeys (see handle_comment_key()). Confirmed with Enter,
     * skipped (armed with no comment) with Esc. The confirmed/skipped
     * text is copied into armed_comment, which is consumed (and cleared)
     * the next time 'space' starts an energy capture -- one-shot, as
     * requested: it applies only to that one test run. */
    int prompting_comment;
    char comment_input[256];
    int comment_input_len;
    char armed_comment[256];

    double cached_voltage;
    double last_voltage_read_t;
    int have_cached_voltage;

    TuiLog log;
} TuiState;

static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double tui_dev_last_voltage(TuiState *st) {
    double now = now_monotonic();
    if (st->have_cached_voltage && now - st->last_voltage_read_t < 1.0 / VOLTAGE_POLL_HZ) {
        return st->cached_voltage;
    }
    double v;
    if (ina3221_read_bus_voltage_v(st->dev, st->channel, &v) == 0) {
        st->cached_voltage = v;
        st->have_cached_voltage = 1;
    }
    st->last_voltage_read_t = now;
    return st->have_cached_voltage ? st->cached_voltage : 0.0;
}

static void *baseline_worker_main(void *arg) {
    TuiState *st = (TuiState *)arg;
    run_baseline_test(st->sampler, st->dev, st->sysmon, 10.0, NULL, &st->baseline_result);
    st->baseline_done_flag = 1;
    return NULL;
}

static void log_test_result_lines(TuiState *st, const TestResult *r) {
    tui_log_write(&st->log, "%s", r->name);
    if (r->has_comment && r->comment[0]) {
        tui_log_write(&st->log, "  comment:       %s", r->comment);
    }
    tui_log_write(&st->log, "  duration:      %.3f s", r->duration_s);
    tui_log_write(&st->log, "  samples:       %ld  (~%.0f Hz)", r->n_samples, r->achieved_hz);
    tui_log_write(&st->log, "  mean current:  %.1f mA", r->mean_current_ma);
    tui_log_write(&st->log, "  min / max:     %.1f / %.1f mA", r->min_current_ma, r->max_current_ma);
    tui_log_write(&st->log, "  bus voltage:   %.3f V", r->bus_voltage_v);
    tui_log_write(&st->log, "  mean power:    %.1f mW", r->mean_power_mw);
    if (r->has_energy) {
        tui_log_write(&st->log, "  energy:        %.3f mWh  (%.3f mAh)", r->mwh, r->mah);
    }
    if (r->has_sysinfo_summary && r->sysinfo_summary.sys_n_samples > 0) {
        const SysSummaryStats *s = &r->sysinfo_summary;
        tui_log_write(&st->log, "  CPU avg/max:   %.1f%% / %.1f%%", s->avg_cpu_percent,
                      s->max_cpu_percent);
        tui_log_write(&st->log, "  GPU avg/max:   %.1f%% / %.1f%%", s->avg_gpu_percent,
                      s->max_gpu_percent);
        if (s->has_temp) {
            tui_log_write(&st->log, "  die temp avg/max: %.1f C / %.1f C", s->avg_temp_c,
                          s->max_temp_c);
        }
    }
    if (r->csv_path) tui_log_write(&st->log, "  raw samples:   %s", r->csv_path);
    if (r->sysinfo_csv_path) tui_log_write(&st->log, "  sysinfo csv:   %s", r->sysinfo_csv_path);
    if (r->jsonl_path) tui_log_write(&st->log, "  logged to:     %s", r->jsonl_path);
}

/* Handles a keystroke while the comment prompt is active (triggered by
 * 'e' -- see the TuiState.prompting_comment field comment). Printable
 * ASCII is appended to comment_input; Backspace/DEL removes the last
 * character; Enter confirms (arms the energy test with whatever text was
 * typed, possibly empty) and Esc cancels the prompt (arms with no
 * comment) -- either way this leaves the normal 'e'-armed state so
 * SPACE still starts the capture as usual. */
static void handle_comment_key(TuiState *st, char key) {
    if (key == '\r' || key == '\n') { /* Enter: confirm */
        st->comment_input[st->comment_input_len] = '\0';
        snprintf(st->armed_comment, sizeof(st->armed_comment), "%s", st->comment_input);
        st->prompting_comment = 0;
        st->energy_armed = 1;
        snprintf(st->mode, sizeof(st->mode), "ENERGY TEST ARMED (space to start)");
        if (st->armed_comment[0]) {
            tui_log_write(&st->log, "Energy test armed with comment: \"%s\"", st->armed_comment);
        } else {
            tui_log_write(&st->log, "Energy test armed (no comment). Press SPACE to start.");
        }
    } else if (key == 0x1b) { /* Esc: cancel -- arm with no comment */
        st->armed_comment[0] = '\0';
        st->prompting_comment = 0;
        st->energy_armed = 1;
        snprintf(st->mode, sizeof(st->mode), "ENERGY TEST ARMED (space to start)");
        tui_log_write(&st->log, "Comment skipped. Energy test armed. Press SPACE to start.");
    } else if (key == 0x7f || key == 0x08) { /* Backspace/DEL */
        if (st->comment_input_len > 0) st->comment_input_len--;
    } else if ((unsigned char)key >= 0x20 && (unsigned char)key < 0x7f) { /* printable ASCII */
        if ((size_t)st->comment_input_len + 1 < sizeof(st->comment_input)) {
            st->comment_input[st->comment_input_len++] = key;
        }
    }
    /* Other control bytes (arrow keys, etc.) are silently ignored while
     * prompting -- this is a plain single-line text field, not a full
     * line editor. */
}

static void handle_key(TuiState *st, char key) {
    if (st->prompting_comment) {
        handle_comment_key(st, key);
        return;
    }
    switch (key) {
        case 'b':
        case 'B':
            if (st->baseline_running || st->energy.active) {
                tui_log_write(&st->log, "[!] Busy -- finish current test first.");
                break;
            }
            snprintf(st->mode, sizeof(st->mode), "BASELINE RUNNING (10s)...");
            st->baseline_running = 1;
            tui_log_write(&st->log, "Baseline test started (10s).");
            pthread_create(&st->baseline_thread, NULL, baseline_worker_main, st);
            st->baseline_thread_joinable = 1;
            break;
        case 'e':
        case 'E':
            if (st->baseline_running) {
                tui_log_write(&st->log, "[!] Busy -- baseline test running.");
                break;
            }
            if (st->energy.active) {
                tui_log_write(&st->log, "[!] Energy capture already active.");
                break;
            }
            st->prompting_comment = 1;
            st->comment_input[0] = '\0';
            st->comment_input_len = 0;
            snprintf(st->mode, sizeof(st->mode), "ENTER COMMENT (Enter=ok, Esc=skip)");
            tui_log_write(&st->log, "Type a comment for this run, then Enter (or Esc to skip).");
            break;
        case ' ':
            if (st->baseline_running) break;
            if (!st->energy.active) {
                energy_capture_start(&st->energy, st->armed_comment);
                st->armed_comment[0] = '\0'; /* one-shot: consumed now */
                snprintf(st->mode, sizeof(st->mode), "ENERGY CAPTURE ACTIVE (space to stop)");
                tui_log_write(&st->log, "Energy capture started.");
            } else {
                TestResult result;
                energy_capture_stop(&st->energy, &result);
                snprintf(st->mode, sizeof(st->mode), "idle");
                tui_log_write(&st->log, "Energy capture stopped.");
                log_test_result_lines(st, &result);
                test_result_free(&result);
            }
            break;
        case 'r':
        case 'R':
            sparkline_history_clear(&st->hist_current);
            sparkline_history_clear(&st->hist_cpu);
            sparkline_history_clear(&st->hist_gpu);
            sparkline_history_clear(&st->hist_temp);
            tui_log_write(&st->log, "Sparkline history cleared.");
            break;
        case 'q':
        case 'Q':
        case 0x03: /* Ctrl-C, delivered as a byte since ISIG is disabled */
            g_tui_stop = 1;
            break;
        default:
            break;
    }
}

/* Reads all currently-available bytes from stdin (non-blocking, since
 * raw mode sets VMIN=0/VTIME=0) and dispatches each as a keypress. */
static void poll_keyboard(TuiState *st) {
    char buf[16];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            handle_key(st, buf[i]);
        }
    }
}

static const char *channel_label(int channel) { return ina3221_channel_name(channel); }

/* Computes how many log lines can be shown given the terminal's current
 * row count, so the log region grows/shrinks with the window instead of
 * a fixed constant -- reserves FIXED_CHROME_ROWS for the status/sparkline/
 * footer rows and clamps to [LOG_VISIBLE_LINES_MIN, LOG_VISIBLE_LINES_MAX]
 * so a very short or very tall terminal still renders sensibly. */
static int compute_log_visible_lines(int term_rows) {
    int available = term_rows - FIXED_CHROME_ROWS;
    if (available < LOG_VISIBLE_LINES_MIN) return LOG_VISIBLE_LINES_MIN;
    if (available > LOG_VISIBLE_LINES_MAX) return LOG_VISIBLE_LINES_MAX;
    return available;
}

static void render(TuiState *st, Screen *scr, int term_rows, int term_cols) {
    screen_begin_frame(scr);
    screen_set_term_width(scr, term_cols);
    int log_visible_lines = compute_log_visible_lines(term_rows);

    int row = 0;

    /* --- Row 0: Mode / Channel / Latest / Rate / Jitter --- */
    Sample latest;
    int have_latest = sampler_peek_latest(st->sampler, &latest);
    SamplerStats stats;
    sampler_get_stats(st->sampler, &stats);

    const char *mode_display = st->mode[0] ? st->mode : "idle";
    if (st->energy.active) mode_display = "ENERGY CAPTURE ACTIVE (space to stop)";

    double voltage = tui_dev_last_voltage(st);
    if (have_latest) {
        screen_set_line(scr, row++,
                         "Mode: %-28s Channel: %-12s Latest: %7.1f mA  %.3f V   Rate: %6.1f Hz   "
                         "Jitter: %6.1f us   Max period: %7.1f us",
                         mode_display, channel_label(st->channel), latest.current_ma, voltage,
                         stats.achieved_hz, stats.jitter_std_us, stats.max_period_us);
    } else {
        screen_set_line(scr, row++,
                         "Mode: %-28s Channel: %-12s Latest: -- mA  %.3f V   Rate: %6.1f Hz",
                         mode_display, channel_label(st->channel), voltage, stats.achieved_hz);
    }

    /* --- Row 1: CPU/GPU/RAM/Swap/Fan/Temp -- OR, while the comment
     * prompt is active, the live comment-input line instead. Reusing
     * this row (rather than adding a new one) keeps FIXED_CHROME_ROWS
     * exact regardless of prompt state. `sys` is read here regardless of
     * prompt state (used below to feed the sparkline history), just not
     * displayed while prompting. */
    SysSnapshot sys;
    sysmon_get_latest(st->sysmon, &sys);
    if (st->prompting_comment) {
        screen_set_line(scr, row++, "Comment: %s_", st->comment_input);
    } else {
        if (sys.ok) {
            char fan_str[32];
            if (sys.fan_rpm >= 0) {
                snprintf(fan_str, sizeof(fan_str), "%.0f RPM", sys.fan_rpm);
            } else {
                snprintf(fan_str, sizeof(fan_str), "--");
            }
            char temp_str[32];
            if (sys.temp_c > -999.0) {
                snprintf(temp_str, sizeof(temp_str), "%.1f C", sys.temp_c);
            } else {
                snprintf(temp_str, sizeof(temp_str), "--");
            }
            screen_set_line(scr, row++,
                             "CPU: %5.1f%%   GPU: %5.1f%%   RAM: %6.0f/%.0f MB (%4.1f%%)   Swap: "
                             "%6.0f/%.0f MB (%4.1f%%)   Fan: %-10s Die temp: %s",
                             sys.cpu_percent, sys.gpu_percent, sys.ram_used_mb, sys.ram_total_mb,
                             sys.ram_percent, sys.swap_used_mb, sys.swap_total_mb,
                             sys.swap_percent, fan_str, temp_str);
        } else {
            screen_set_line(scr, row++, "System: unavailable");
        }
    }

    /* --- Sparkline rows: each metric gets SPARKLINE_ROWS stacked rows of
     * glyphs (see sparkline.h's sparkline_render_rows() for how the
     * levels are distributed bottom-up across rows) -- the label/value
     * text sits on the FIRST row, left-padded blank on subsequent rows so
     * the glyph columns still line up vertically. The pad width for
     * continuation rows MUST equal the first row's actual printed prefix
     * length -- computed per-metric via snprintf into a scratch buffer
     * rather than a single hardcoded constant, since "%%" literal percent
     * signs in the CPU/GPU format strings make their prefixes a
     * different length than Current's (a bug found via a user screenshot:
     * a stray `%%` collapsing to one `%` character shortened those three
     * prefixes by 1 column vs. a hardcoded label_width=23, visibly
     * shifting their top sparkline row left by one column relative to
     * the bottom row). */
    char prefix_buf[64];
    int prefix_len;
    int spark_width; /* recomputed per-metric from that metric's own
                         actual prefix_len right below, so the sparkline
                         always fills the terminal to the same right
                         edge regardless of small prefix-length
                         differences between metrics */

    char spark_bufs[SPARKLINE_ROWS][SPARKLINE_COLOR_ROW_CAP];
    char *spark_rows[SPARKLINE_ROWS];
    for (int r = 0; r < SPARKLINE_ROWS; r++) spark_rows[r] = spark_bufs[r];

    double latest_current_ma = have_latest ? latest.current_ma : 0.0;
    if (have_latest) sparkline_history_push(&st->hist_current, latest_current_ma);
    prefix_len = snprintf(prefix_buf, sizeof(prefix_buf), "Current (mA): %7.1f  ", latest_current_ma);
    spark_width = term_cols - prefix_len;
    if (spark_width < 10) spark_width = 10;
    if (spark_width > 200) spark_width = 200;
    sparkline_render_rows_color(&st->hist_current, spark_width, SPARKLINE_ROWS, spark_rows,
                                 sizeof(spark_bufs[0]),
                                 st->use_color ? &SPARKLINE_COLOR_CURRENT : NULL);
    screen_set_line(scr, row++, "%s%s", prefix_buf, spark_rows[0]);
    for (int r = 1; r < SPARKLINE_ROWS; r++) {
        screen_set_line(scr, row++, "%-*s%s", prefix_len, "", spark_rows[r]);
    }

    if (sys.ok) {
        sparkline_history_push(&st->hist_cpu, sys.cpu_percent);
        sparkline_history_push(&st->hist_gpu, sys.gpu_percent);
        if (sys.temp_c > -999.0) sparkline_history_push(&st->hist_temp, sys.temp_c);
    }

    prefix_len = snprintf(prefix_buf, sizeof(prefix_buf), "CPU load (%%): %6.1f  ",
                           sys.ok ? sys.cpu_percent : 0.0);
    spark_width = term_cols - prefix_len;
    if (spark_width < 10) spark_width = 10;
    if (spark_width > 200) spark_width = 200;
    sparkline_render_rows_color(&st->hist_cpu, spark_width, SPARKLINE_ROWS, spark_rows,
                                 sizeof(spark_bufs[0]), st->use_color ? &SPARKLINE_COLOR_CPU : NULL);
    screen_set_line(scr, row++, "%s%s", prefix_buf, spark_rows[0]);
    for (int r = 1; r < SPARKLINE_ROWS; r++) {
        screen_set_line(scr, row++, "%-*s%s", prefix_len, "", spark_rows[r]);
    }

    prefix_len = snprintf(prefix_buf, sizeof(prefix_buf), "GPU load (%%): %6.1f  ",
                           sys.ok ? sys.gpu_percent : 0.0);
    spark_width = term_cols - prefix_len;
    if (spark_width < 10) spark_width = 10;
    if (spark_width > 200) spark_width = 200;
    sparkline_render_rows_color(&st->hist_gpu, spark_width, SPARKLINE_ROWS, spark_rows,
                                 sizeof(spark_bufs[0]), st->use_color ? &SPARKLINE_COLOR_GPU : NULL);
    screen_set_line(scr, row++, "%s%s", prefix_buf, spark_rows[0]);
    for (int r = 1; r < SPARKLINE_ROWS; r++) {
        screen_set_line(scr, row++, "%-*s%s", prefix_len, "", spark_rows[r]);
    }

    prefix_len = snprintf(prefix_buf, sizeof(prefix_buf), "Die temp (C): %6.1f  ",
                           (sys.ok && sys.temp_c > -999.0) ? sys.temp_c : 0.0);
    spark_width = term_cols - prefix_len;
    if (spark_width < 10) spark_width = 10;
    if (spark_width > 200) spark_width = 200;
    sparkline_render_rows_color(&st->hist_temp, spark_width, SPARKLINE_ROWS, spark_rows,
                                 sizeof(spark_bufs[0]),
                                 st->use_color ? &SPARKLINE_COLOR_TEMP : NULL);
    screen_set_line(scr, row++, "%s%s", prefix_buf, spark_rows[0]);
    for (int r = 1; r < SPARKLINE_ROWS; r++) {
        screen_set_line(scr, row++, "%-*s%s", prefix_len, "", spark_rows[r]);
    }

    /* --- Blank separator + log tail --- */
    screen_set_line(scr, row++, "%s", "");
    const char *recent[LOG_VISIBLE_LINES_MAX];
    int n_recent = tui_log_recent(&st->log, recent, log_visible_lines);
    for (int i = 0; i < n_recent; i++) {
        screen_set_line(scr, row++, "%s", recent[i]);
    }

    /* --- Footer: keybindings --- */
    if (st->prompting_comment) {
        screen_set_line(scr, row++, "%s", "Type your comment, Enter to confirm, Esc to skip");
    } else {
        screen_set_line(scr, row++,
                         "%s",
                         "b: baseline(10s)  e: arm energy  space: start/stop  r: reset  q: quit");
    }

    screen_flush(scr);
}

int run_tui(Ina3221 *dev, Sampler *sampler, SysMonitor *sysmon, int channel, int use_color) {
    /* static, not stack-local: Screen (screen.h) is ~1MB with
     * SCREEN_LINE_CAP sized for worst-case truecolor escape sequences --
     * keeping both off the stack avoids any risk of stack overflow
     * regardless of the calling thread's stack size. run_tui() is only
     * ever called once per process (no re-entrancy needed). */
    static TuiState st;
    static Screen scr;
    memset(&st, 0, sizeof(st));
    st.dev = dev;
    st.sampler = sampler;
    st.sysmon = sysmon;
    st.channel = channel;
    st.use_color = use_color;
    sparkline_history_init(&st.hist_current);
    sparkline_history_init(&st.hist_cpu);
    sparkline_history_init(&st.hist_gpu);
    sparkline_history_init(&st.hist_temp);
    snprintf(st.mode, sizeof(st.mode), "idle");
    tui_log_init(&st.log);
    energy_capture_init(&st.energy, sampler, dev, sysmon);

    tui_log_write(&st.log, "Jetson Energy Usage (native) -- INA3221 sampler started.");
    tui_log_write(&st.log,
                  "Press 'b' for a 10s baseline test, 'e' then space to start/stop an "
                  "energy capture, 'q' to quit.");

    signal(SIGINT, on_sigint_tui);
    term_enable_raw_mode();
    term_enter_alt_screen();
    term_hide_cursor();

    screen_init(&scr);

    double period = 1.0 / UI_REFRESH_HZ;
    g_tui_stop = 0;

    while (!g_tui_stop) {
        double tick_start = now_monotonic();

        poll_keyboard(&st);
        if (g_tui_stop) break;

        if (st.baseline_running && st.baseline_done_flag) {
            pthread_join(st.baseline_thread, NULL);
            st.baseline_thread_joinable = 0;
            st.baseline_running = 0;
            st.baseline_done_flag = 0;
            snprintf(st.mode, sizeof(st.mode), "idle");
            tui_log_write(&st.log, "Baseline test stopped.");
            log_test_result_lines(&st, &st.baseline_result);
            test_result_free(&st.baseline_result);
        }

        if (st.energy.active) {
            energy_capture_poll(&st.energy);
        } else if (!st.baseline_running) {
            /* Idle: drain the sampler periodically so its ring buffer
             * doesn't silently grow toward its cap while no test is
             * running (mirrors the Python TUI's idle-drain behavior). */
            SampleList discard;
            sample_list_init(&discard);
            sampler_drain(sampler, &discard);
            sample_list_free(&discard);
        }

        int rows, cols;
        term_get_size(&rows, &cols);
        render(&st, &scr, rows, cols);

        double elapsed = now_monotonic() - tick_start;
        double remaining = period - elapsed;
        if (remaining > 0) {
            struct timespec ts;
            ts.tv_sec = (time_t)remaining;
            ts.tv_nsec = (long)((remaining - ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
    }

    if (st.baseline_thread_joinable) {
        /* Best-effort: the baseline test has a fixed 10s duration and
         * doesn't check a stop flag mid-run (matches the Python worker's
         * behavior -- 'q' during a baseline test waits for it like the
         * Python TUI would too), so just join. */
        pthread_join(st.baseline_thread, NULL);
        test_result_free(&st.baseline_result);
    }
    if (st.energy.active) {
        TestResult discard_result;
        energy_capture_stop(&st.energy, &discard_result);
        test_result_free(&discard_result);
    }
    energy_capture_free(&st.energy);

    term_show_cursor();
    term_leave_alt_screen();
    term_restore_mode();

    return 0;
}
