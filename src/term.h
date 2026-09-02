/*
 * term.h -- minimal raw-terminal helpers: raw/cbreak mode, alternate
 * screen buffer, cursor show/hide, terminal size query, and small ANSI
 * escape-sequence wrappers. No ncurses/terminfo dependency -- see
 * README.md's TUI section for why (fixed known layout, no benefit from
 * terminfo portability here).
 */
#ifndef JEU_TERM_H
#define JEU_TERM_H

/* Puts stdin into full raw mode (no ICANON, no ECHO, no ISIG -- Ctrl-C is
 * delivered as byte 0x03 in the input stream, not a signal, so the TUI's
 * own key-handling loop can treat it as "quit" and restore the terminal
 * cleanly rather than the process dying mid-frame). No-op if stdin isn't
 * a tty. Safe to call once; call term_restore_mode() to undo. */
void term_enable_raw_mode(void);

/* Restores whatever termios state was active before term_enable_raw_mode()
 * was called. Safe to call even if raw mode was never enabled. */
void term_restore_mode(void);

/* Switches to the terminal's alternate screen buffer (like vim/less) so
 * the TUI doesn't scroll the user's normal scrollback -- and
 * term_leave_alt_screen() restores it, revealing whatever was on screen
 * before. */
void term_enter_alt_screen(void);
void term_leave_alt_screen(void);

void term_hide_cursor(void);
void term_show_cursor(void);

/* Fills the row/col outputs with the current terminal size via
 * ioctl(TIOCGWINSZ). Falls back to 24x80 if the ioctl fails (e.g. stdout
 * redirected to a file) so the TUI can still render *something* rather
 * than divide by zero. */
void term_get_size(int *rows, int *cols);

#endif /* JEU_TERM_H */
