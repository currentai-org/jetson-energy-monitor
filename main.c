/*
 * main.c -- Jetson Energy Usage CLI/TUI (jeu). CLI argument parsing and
 * mode dispatch: `tui` (interactive, default), `baseline` (waits
 * --duration seconds collecting current, then prints/logs results), and
 * `energy` (starts capturing immediately, stops on any keypress or
 * Ctrl-C, then prints/logs results).
 *
 * See README.md for usage and docs/DEVLOG.md for design history.
 *
 * Usage:
 *   jeu [tui|baseline|energy] [--hz N] [--duration SEC] [--channel NAME]
 *
 * Build: see Makefile.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "ina3221.h"
#include "sampler.h"
#include "sysmon.h"
#include "tests.h"
#include "tui.h"

#define DEFAULT_SAMPLE_HZ 1000.0
#define DEFAULT_SYSINFO_HZ 2.0
#define DEFAULT_DURATION_S 10.0
#define I2C_BUS_NUM 1
#define I2C_ADDRESS 0x40
#define SHUNT_OHMS 0.005
#define ENERGY_POLL_HZ 10.0

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signum) {
    (void)signum;
    g_stop = 1;
}

/* --- raw/cbreak terminal mode for the energy test's "press any key to
   stop" behavior -- mirrors app.py's _raw_terminal() context manager. --- */

static struct termios g_orig_termios;
static int g_termios_saved = 0;

static void restore_terminal(void) {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_orig_termios);
        g_termios_saved = 0;
    }
}

static void enable_cbreak_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return;
    g_termios_saved = 1;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
}

/* Returns 1 if stdin has input ready within `timeout_s` seconds, mirroring
 * app.py's _key_pressed(). */
static int key_pressed(double timeout_s) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv;
    tv.tv_sec = (long)timeout_s;
    tv.tv_usec = (long)((timeout_s - tv.tv_sec) * 1e6);
    int rc = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    return rc > 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [tui|baseline|energy] [--hz N] [--duration SEC] [--channel NAME] "
            "[--color|--no-color]\n"
            "\n"
            "  tui                Interactive terminal UI with live sparklines (current/\n"
            "                     CPU/GPU/temp) -- keybindings b/e/space/r/q (see README).\n"
            "                     This is the default when no test name is given.\n"
            "  baseline           Waits (idle) --duration seconds (default %.0f) collecting\n"
            "                     current samples, then prints/logs a results summary.\n"
            "  energy             Starts capturing immediately; press any key to stop\n"
            "                     (falls back to Ctrl-C if stdin isn't an interactive\n"
            "                     terminal), then prints/logs a results summary.\n"
            "  --hz N             Target INA3221 current sampling rate in Hz (default %.0f).\n"
            "  --duration SEC     Duration in seconds for 'baseline' (default %.0f). Ignored\n"
            "                     for 'energy'/'tui'.\n"
            "  --channel NAME     INA3221 rail to sample: VDD_IN, VDD_CPU_GPU, or VDD_SOC\n"
            "                     (default VDD_IN).\n"
            "  --sysinfo-hz N     Poll rate in Hz for CPU/GPU/RAM/swap/fan/temp (default %.0f).\n"
            "  --color            Force-enable truecolor sparkline gradients in 'tui' mode\n"
            "                     (green->per-metric color: current=yellow, cpu=cyan,\n"
            "                     gpu=magenta, temp=red). On by default when stdout is a\n"
            "                     terminal and NO_COLOR/TERM=dumb aren't set.\n"
            "  --no-color         Disable sparkline colors -- plain glyphs only. Use this if\n"
            "                     your terminal/multiplexer doesn't render truecolor (24-bit\n"
            "                     RGB) escape codes well; falls back to a lower-fidelity but\n"
            "                     universally-readable display. Ignored for 'baseline'/\n"
            "                     'energy' (no color output there regardless).\n",
            prog, DEFAULT_DURATION_S, DEFAULT_SAMPLE_HZ, DEFAULT_DURATION_S, DEFAULT_SYSINFO_HZ);
}

/* Picks the --color/--no-color default when neither flag is passed
 * explicitly: on by default, but auto-disabled for environments where
 * truecolor escape codes are unlikely to render well or could corrupt
 * non-terminal output -- same conventions common CLI tools (git, ls
 * --color=auto, ripgrep) use. */
static int default_color_enabled(void) {
    if (!isatty(STDOUT_FILENO)) return 0;      /* piped/redirected output */
    if (getenv("NO_COLOR") != NULL) return 0;  /* https://no-color.org */
    const char *term = getenv("TERM");
    if (term != NULL && strcmp(term, "dumb") == 0) return 0;
    return 1;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    const char *test = "tui"; /* default: interactive TUI, no args needed */
    int arg_start = 1;

    if (argc >= 2 && argv[1][0] != '-') {
        test = argv[1];
        arg_start = 2;
    }
    if (strcmp(test, "tui") != 0 && strcmp(test, "baseline") != 0 && strcmp(test, "energy") != 0) {
        fprintf(stderr, "unknown test %s; expected 'tui', 'baseline', or 'energy'\n", test);
        print_usage(argv[0]);
        return 1;
    }

    double sample_hz = DEFAULT_SAMPLE_HZ;
    double sysinfo_hz = DEFAULT_SYSINFO_HZ;
    double duration_s = DEFAULT_DURATION_S;
    char channel_name_buf[32] = "VDD_IN";
    int use_color = default_color_enabled(); /* may be overridden below */

    for (int i = arg_start; i < argc; i++) {
        if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc) {
            sample_hz = atof(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_s = atof(argv[++i]);
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            snprintf(channel_name_buf, sizeof(channel_name_buf), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--sysinfo-hz") == 0 && i + 1 < argc) {
            sysinfo_hz = atof(argv[++i]);
        } else if (strcmp(argv[i], "--color") == 0) {
            use_color = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            use_color = 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    int channel = ina3221_channel_from_name(channel_name_buf);
    if (channel == 0) {
        fprintf(stderr, "unknown channel name %s; expected VDD_IN, VDD_CPU_GPU, or VDD_SOC\n",
                channel_name_buf);
        return 1;
    }

    signal(SIGINT, on_sigint);

    Ina3221 dev;
    if (ina3221_open(&dev, I2C_BUS_NUM, I2C_ADDRESS, SHUNT_OHMS) != 0) {
        fprintf(stderr, "failed to open I2C bus %d: %s\n", I2C_BUS_NUM, strerror(errno));
        return 1;
    }
    if (ina3221_configure_fast_single_channel(&dev, channel) != 0) {
        fprintf(stderr, "failed to configure INA3221: %s\n", strerror(errno));
        ina3221_close(&dev);
        return 1;
    }

    Sampler sampler;
    if (sampler_init(&sampler, &dev, channel, sample_hz) != 0) {
        fprintf(stderr, "failed to allocate sampler buffers\n");
        ina3221_restore_config(&dev);
        ina3221_close(&dev);
        return 1;
    }

    SysMonitor sysmon;
    sysmon_init(&sysmon, sysinfo_hz);

    if (sampler_start(&sampler) != 0) {
        fprintf(stderr, "failed to start sampler thread\n");
        goto cleanup;
    }
    if (sysmon_start(&sysmon) != 0) {
        fprintf(stderr, "failed to start sysmon thread\n");
        goto cleanup;
    }

    const char *chan_label = ina3221_channel_name(channel);

    if (strcmp(test, "tui") == 0) {
        int rc = run_tui(&dev, &sampler, &sysmon, channel, use_color);
        sysmon_stop(&sysmon);
        sysmon_destroy(&sysmon);
        sampler_stop(&sampler);
        sampler_free(&sampler);
        ina3221_restore_config(&dev);
        ina3221_close(&dev);
        return rc;
    }

    TestResult result;
    if (strcmp(test, "baseline") == 0) {
        printf("Baseline (idle) test on %s: collecting for %.0fs...\n", chan_label, duration_s);
        run_baseline_test(&sampler, &dev, &sysmon, duration_s, &result);
    } else {
        EnergyCapture ec;
        energy_capture_init(&ec, &sampler, &dev, &sysmon);
        energy_capture_start(&ec);

        int stdin_is_tty = isatty(STDIN_FILENO);
        if (stdin_is_tty) {
            printf("Energy capture started on %s. Press any key to stop...\n", chan_label);
            enable_cbreak_mode();
        } else {
            printf("Energy capture started on %s. stdin is not a terminal -- press Ctrl-C to "
                   "stop...\n",
                   chan_label);
        }
        fflush(stdout);

        double poll_period = 1.0 / ENERGY_POLL_HZ;
        while (!g_stop) {
            if (stdin_is_tty && key_pressed(poll_period)) {
                char discard_buf[8];
                ssize_t n_read = read(STDIN_FILENO, discard_buf, sizeof(discard_buf));
                (void)n_read; /* consume keypress; nothing to do with the byte(s) read */
                break;
            } else if (!stdin_is_tty) {
                /* No tty: just wait for SIGINT, sleeping poll_period at a
                 * time so energy_capture_poll() still runs regularly to
                 * drain the sampler's ring buffer. */
                struct timespec ts;
                ts.tv_sec = (time_t)poll_period;
                ts.tv_nsec = (long)((poll_period - ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
            }
            energy_capture_poll(&ec);
        }
        restore_terminal();

        energy_capture_stop(&ec, &result);
        energy_capture_free(&ec);
    }

    printf("\n");
    test_result_print_summary(&result);
    test_result_free(&result);

cleanup:
    restore_terminal();
    sysmon_stop(&sysmon);
    sysmon_destroy(&sysmon);
    sampler_stop(&sampler);
    sampler_free(&sampler);
    ina3221_restore_config(&dev);
    ina3221_close(&dev);
    return 0;
}
