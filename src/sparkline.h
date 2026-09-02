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

/* Low/high truecolor endpoints for a gradient sparkline. Matches the
 * Python Textual TUI's per-metric max colors (see app.py CSS:
 * #spark-current -> yellow, #spark-cpu -> cyan, #spark-gpu -> magenta,
 * #spark-temp -> red), each paired with Textual Sparkline's default
 * green "min" color -- see SPARKLINE_COLOR_* constants below. */
typedef struct {
    unsigned char r_lo, g_lo, b_lo; /* color at the bottom/lowest value */
    unsigned char r_hi, g_hi, b_hi; /* color at the top/highest value */
} SparklineColorScheme;

/* Preset schemes matching the old Python TUI's per-metric colors
 * (green -> metric color), for direct use as sparkline_render_rows_color()
 * arguments. */
extern const SparklineColorScheme SPARKLINE_COLOR_CURRENT; /* green -> yellow */
extern const SparklineColorScheme SPARKLINE_COLOR_CPU;     /* green -> cyan */
extern const SparklineColorScheme SPARKLINE_COLOR_GPU;     /* green -> magenta */
extern const SparklineColorScheme SPARKLINE_COLOR_TEMP;    /* green -> red */

/* Same as sparkline_render_rows(), but wraps glyphs in 24-bit truecolor
 * ANSI SGR escapes (`\x1b[38;2;R;G;Bm`) that interpolate between
 * `scheme->{r,g,b}_lo` (at the bottom of the value's rendered range) and
 * `scheme->{r,g,b}_hi` (at the top), giving a continuous color "channel"
 * of information on top of the existing block-height levels -- a
 * genuinely free complementary technique to the row-stacking done by
 * sparkline_render_rows() (see native/README.md's "Vertical resolution"
 * section, option 3). To keep the byte cost low, escape codes are only
 * emitted when the color actually changes between adjacent glyphs in the
 * SAME row (samples at the same color bucket share one escape), not once
 * per glyph, and a single trailing `\x1b[0m` resets state at the end of
 * each row's string. Pass scheme=NULL to render with no color (falls
 * back to plain glyphs, identical output to sparkline_render_rows()) --
 * this is how the --no-color flag is implemented. Output buffers need to
 * be somewhat larger than the colorless version to hold escape
 * sequences; see SPARKLINE_COLOR_ROW_CAP. */
void sparkline_render_rows_color(const SparklineHistory *h, int width, int n_rows,
                                  char **out_rows, size_t out_cap,
                                  const SparklineColorScheme *scheme);

/* Suggested minimum per-row output buffer size when colors may be
 * enabled: worst case is one truecolor escape (up to ~20 bytes) per
 * glyph column if every adjacent pair of samples lands in a different
 * color bucket, plus one trailing reset (4 bytes), plus the glyph bytes
 * themselves (up to 3 bytes each). Generous headroom for a ~200-column
 * terminal. */
#define SPARKLINE_COLOR_ROW_CAP 4096

#endif /* JEU_SPARKLINE_H */
