/* screen.c -- see screen.h */
#include "screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void screen_init(Screen *scr) {
    memset(scr, 0, sizeof(*scr));
    scr->first_frame = 1;
}

void screen_begin_frame(Screen *scr) {
    /* Nothing to reset structurally -- screen_set_line overwrites
     * scr->current[row] directly each tick; n_lines_used is updated as
     * rows are set. */
    (void)scr;
}

void screen_set_term_width(Screen *scr, int cols) {
    scr->term_cols = cols;
}

/* Truncates `line` in place to at most `max_cols` *display columns*,
 * treating each of our sparkline glyphs (3-byte UTF-8 sequences, always
 * exactly 1 display column wide -- see sparkline.c) as a single column
 * rather than 3, so a line isn't cut mid-glyph and column budgeting
 * matches what the terminal will actually show. Safe on plain ASCII
 * lines too (each byte = 1 column there). */
static void truncate_line_to_columns(char *line, int max_cols) {
    if (max_cols < 0) max_cols = 0;
    int col = 0;
    unsigned char *p = (unsigned char *)line;
    while (*p && col < max_cols) {
        if ((*p & 0xE0) == 0xC0) {
            p += 2; /* 2-byte UTF-8 sequence */
        } else if ((*p & 0xF0) == 0xE0) {
            p += 3; /* 3-byte UTF-8 sequence (our sparkline glyphs) */
        } else if ((*p & 0xF8) == 0xF0) {
            p += 4; /* 4-byte UTF-8 sequence */
        } else {
            p += 1; /* plain ASCII byte */
        }
        col++;
    }
    *p = '\0';
}

void screen_set_line(Screen *scr, int row, const char *fmt, ...) {
    if (row < 0 || row >= SCREEN_MAX_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(scr->current[row], SCREEN_LINE_CAP, fmt, ap);
    va_end(ap);
    if (scr->term_cols > 0) {
        truncate_line_to_columns(scr->current[row], scr->term_cols);
    }
    if (row + 1 > scr->n_lines_used) scr->n_lines_used = row + 1;
}

void screen_flush(Screen *scr) {
    /* Build one combined write buffer for all changed lines, then a
     * single fwrite/fflush -- avoids one write() syscall per changed
     * line (same batching rationale as csv_log.c's buffered CSV writes). */
    static char out_buf[SCREEN_MAX_LINES * (SCREEN_LINE_CAP + 32)];
    size_t pos = 0;

    for (int row = 0; row < scr->n_lines_used; row++) {
        int changed = scr->first_frame || strcmp(scr->current[row], scr->previous[row]) != 0;
        scr->line_dirty[row] = changed;
        if (!changed) continue;

        /* Move cursor to (row+1, col 1), clear to end of line, write text. */
        int n = snprintf(out_buf + pos, sizeof(out_buf) - pos, "\x1b[%d;1H\x1b[2K%s", row + 1,
                          scr->current[row]);
        if (n > 0) pos += (size_t)n;
        if (pos >= sizeof(out_buf) - SCREEN_LINE_CAP - 32) break; /* safety margin */
    }

    if (pos > 0) {
        fwrite(out_buf, 1, pos, stdout);
        fflush(stdout);
    }

    memcpy(scr->previous, scr->current, sizeof(scr->previous));
    scr->first_frame = 0;
}
