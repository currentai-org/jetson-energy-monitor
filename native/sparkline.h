/*
 * sparkline.h -- 8-level Unicode block-character sparklines, matching the
 * visual fidelity of Textual's Sparkline widget (which the Python TUI
 * used) at a fraction of the rendering cost: this is just a string of
 * precomputed UTF-8 glyphs, no widget tree/diffing framework underneath.
 *
 * Level glyphs (U+2581 LOWER ONE EIGHTH BLOCK .. U+2588 FULL BLOCK), the
 * same set used by holman/spark and similar terminal sparkline tools.
 */
#ifndef JEU_SPARKLINE_H
#define JEU_SPARKLINE_H

#include <stddef.h>

#define SPARKLINE_HISTORY 120 /* ~24s of history at 5Hz UI tick, matches
                                  the Python TUI's SPARKLINE_HISTORY */

typedef struct {
    double values[SPARKLINE_HISTORY];
    int count;     /* how many slots are valid so far (caps at HISTORY) */
    int write_idx; /* next slot to write (ring buffer) */
} SparklineHistory;

void sparkline_history_init(SparklineHistory *h);
void sparkline_history_push(SparklineHistory *h, double value);
void sparkline_history_clear(SparklineHistory *h);

/* Renders up to `width` most-recent values from `h` as a UTF-8 string of
 * block glyphs into `out` (must be at least width*3 + 1 bytes -- each
 * glyph is a 3-byte UTF-8 sequence in this codepoint range). Scaling is
 * per-call min/max over the values actually rendered (matches Textual
 * Sparkline's auto-ranging behavior) -- a flat/constant series renders as
 * a flat mid-level line, not division-by-zero. Returns the number of
 * bytes written (excluding the NUL terminator). */
size_t sparkline_render(const SparklineHistory *h, int width, char *out, size_t out_cap);

#define SPARKLINE_MAX_ROWS 4 /* generous headroom; 2 is what's used today */

/* Renders the same history as sparkline_render(), but split across
 * `n_rows` stacked terminal rows instead of one, quadrupling (2 rows) or
 * more the effective vertical resolution: each row still only has 8
 * block levels of its own, but a value's fill height is distributed
 * across rows bottom-up (like a vertical bar chart split across
 * multiple lines of text), giving n_rows*8 distinguishable levels
 * instead of 8. out_rows[0] is the TOP row, out_rows[n_rows-1] is the
 * BOTTOM row (matches how they should be printed top-to-bottom on
 * screen). Each out_rows[i] must point to a buffer of at least
 * width*3 + 1 bytes, same sizing rule as sparkline_render() (out_cap is
 * that per-row buffer size). `n_rows` must be between 1 and
 * SPARKLINE_MAX_ROWS. */
void sparkline_render_rows(const SparklineHistory *h, int width, int n_rows, char **out_rows,
                            size_t out_cap);

#endif /* JEU_SPARKLINE_H */
