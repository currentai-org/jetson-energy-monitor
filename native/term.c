/* term.c -- see term.h */
#include "term.h"

#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_orig_termios;
static int g_termios_saved = 0;

void term_enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return;
    g_termios_saved = 1;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
}

void term_restore_mode(void) {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_orig_termios);
        g_termios_saved = 0;
    }
}

void term_enter_alt_screen(void) {
    fputs("\x1b[?1049h", stdout);
    fflush(stdout);
}

void term_leave_alt_screen(void) {
    fputs("\x1b[?1049l", stdout);
    fflush(stdout);
}

void term_hide_cursor(void) {
    fputs("\x1b[?25l", stdout);
    fflush(stdout);
}

void term_show_cursor(void) {
    fputs("\x1b[?25h", stdout);
    fflush(stdout);
}

void term_get_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}
