/*
 * screen.h -- minimal double-buffered terminal renderer: build a frame as
 * an array of fixed-row text lines, and screen_flush() only emits ANSI
 * cursor-position + write for the lines that actually changed since the
 * last flush (byte-compare against the previous frame). This is the same
 * "damage diffing" idea ncurses/Textual do internally, hand-rolled for
 * our fixed, known layout so we don't need either dependency.
 *
 * Usage per tick:
 *   screen_begin_frame(&scr);
 *   screen_set_line(&scr, 0, "...status text...");
 *   screen_set_line(&scr, 1, "...sparkline row...");
 *   ...
 *   screen_flush(&scr);   // only touches changed lines
 */
#ifndef JEU_SCREEN_H
#define JEU_SCREEN_H

#include <stddef.h>

#define SCREEN_MAX_LINES 64
#define SCREEN_LINE_CAP 512 /* bytes per line, generous for UTF-8 sparkline rows */

typedef struct {
    char current[SCREEN_MAX_LINES][SCREEN_LINE_CAP];
    char previous[SCREEN_MAX_LINES][SCREEN_LINE_CAP];
    int line_dirty[SCREEN_MAX_LINES]; /* set by screen_set_line if changed */
    int n_lines_used;
    int first_frame; /* forces every line to redraw once */
    int term_cols;   /* current terminal width; lines are truncated to this
                         many display columns so nothing wraps onto the
                         next physical row and breaks our fixed-row-per-
                         logical-line assumption. 0 = no truncation. */
} Screen;

void screen_init(Screen *scr);

/* Updates the terminal width used to truncate subsequent screen_set_line
 * calls this frame. Call once per tick before any screen_set_line calls
 * (main.c/tui.c already does this via term_get_size()). */
void screen_set_term_width(Screen *scr, int cols);

/* Call once at the start of each render tick before any screen_set_line
 * calls. Clears the "current" buffer's line count (does not touch
 * "previous" -- that's the diff baseline). */
void screen_begin_frame(Screen *scr);

/* Sets row `row` (0-indexed) of the current frame to `text` (a
 * printf-style format string). Text should not contain embedded newlines;
 * one row = one terminal line. Rows are 1-based when emitted (terminal
 * cursor addressing starts at row 1), translated internally. */
void screen_set_line(Screen *scr, int row, const char *fmt, ...);

/* Compares the current frame to the previous one and writes ANSI cursor-
 * position + clear-to-eol + text for only the rows that changed, then
 * copies current -> previous for the next tick. Ends with a single
 * fflush(stdout). */
void screen_flush(Screen *scr);

#endif /* JEU_SCREEN_H */
