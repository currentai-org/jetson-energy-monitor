/* sparkline.c -- see sparkline.h */
#include "sparkline.h"

#include <string.h>

/* 8 levels, U+2581 (LOWER ONE EIGHTH BLOCK) through U+2588 (FULL BLOCK).
 * All are 3-byte UTF-8 sequences (E2 96 8x). Precomputed as raw bytes to
 * avoid depending on the compiler's source-file encoding for wide
 * character literals. */
static const char *LEVELS[8] = {
    "\xe2\x96\x81", /* ▁ */
    "\xe2\x96\x82", /* ▂ */
    "\xe2\x96\x83", /* ▃ */
    "\xe2\x96\x84", /* ▄ */
    "\xe2\x96\x85", /* ▅ */
    "\xe2\x96\x86", /* ▆ */
    "\xe2\x96\x87", /* ▇ */
    "\xe2\x96\x88", /* █ */
};

void sparkline_history_init(SparklineHistory *h) {
    memset(h, 0, sizeof(*h));
}

void sparkline_history_push(SparklineHistory *h, double value) {
    h->values[h->write_idx] = value;
    h->write_idx = (h->write_idx + 1) % SPARKLINE_HISTORY;
    if (h->count < SPARKLINE_HISTORY) h->count++;
}

void sparkline_history_clear(SparklineHistory *h) {
    h->count = 0;
    h->write_idx = 0;
}

size_t sparkline_render(const SparklineHistory *h, int width, char *out, size_t out_cap) {
    if (width <= 0 || h->count == 0) {
        out[0] = '\0';
        return 0;
    }
    int n = (width < h->count) ? width : h->count;

    /* Oldest-of-the-window index: values are stored in a ring buffer
     * where write_idx is the next slot to be overwritten, so the most
     * recently written value is at (write_idx - 1 + HISTORY) % HISTORY,
     * and we want the last `n` values in chronological order. */
    int start = (h->write_idx - n + SPARKLINE_HISTORY) % SPARKLINE_HISTORY;

    double lo = h->values[start], hi = h->values[start];
    for (int i = 1; i < n; i++) {
        int idx = (start + i) % SPARKLINE_HISTORY;
        double v = h->values[idx];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }

    double range = hi - lo;
    size_t pos = 0;
    /* Left-pad with blanks if we have fewer than `width` samples yet, so
     * the sparkline visually fills in from the right as data accrues
     * (matches Textual Sparkline's behavior with a short data list). */
    int pad = width - n;
    for (int i = 0; i < pad && pos + 1 < out_cap; i++) {
        out[pos++] = ' ';
    }
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % SPARKLINE_HISTORY;
        double v = h->values[idx];
        int level;
        if (range <= 0.0) {
            level = 3; /* flat series: render at a mid-level, not level 0 */
        } else {
            level = (int)(((v - lo) / range) * 7.0 + 0.5);
            if (level < 0) level = 0;
            if (level > 7) level = 7;
        }
        const char *glyph = LEVELS[level];
        size_t glyph_len = 3;
        if (pos + glyph_len >= out_cap) break;
        memcpy(out + pos, glyph, glyph_len);
        pos += glyph_len;
    }
    out[pos] = '\0';
    return pos;
}

/* Shared setup for both sparkline_render() and sparkline_render_rows():
 * figures out how many samples are in the render window, the
 * chronological start index into the ring buffer, and the window's
 * min/max. Returns the sample count actually available (<= width). */
static int sparkline_window(const SparklineHistory *h, int width, int *out_start, double *out_lo,
                             double *out_hi) {
    if (width <= 0 || h->count == 0) return 0;
    int n = (width < h->count) ? width : h->count;
    int start = (h->write_idx - n + SPARKLINE_HISTORY) % SPARKLINE_HISTORY;

    double lo = h->values[start], hi = h->values[start];
    for (int i = 1; i < n; i++) {
        int idx = (start + i) % SPARKLINE_HISTORY;
        double v = h->values[idx];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    *out_start = start;
    *out_lo = lo;
    *out_hi = hi;
    return n;
}

void sparkline_render_rows(const SparklineHistory *h, int width, int n_rows, char **out_rows,
                            size_t out_cap) {
    if (n_rows < 1) n_rows = 1;
    if (n_rows > SPARKLINE_MAX_ROWS) n_rows = SPARKLINE_MAX_ROWS;

    if (width <= 0) {
        for (int r = 0; r < n_rows; r++) out_rows[r][0] = '\0';
        return;
    }

    int start;
    double lo, hi;
    int n = sparkline_window(h, width, &start, &lo, &hi);
    if (n == 0) {
        for (int r = 0; r < n_rows; r++) out_rows[r][0] = '\0';
        return;
    }

    double range = hi - lo;
    int total_levels = n_rows * 8; /* 0 = fully empty, total_levels = fully full */
    int pad = width - n;

    size_t pos[SPARKLINE_MAX_ROWS];
    for (int r = 0; r < n_rows; r++) pos[r] = 0;

    /* Left-pad every row with blanks so a short history still fills in
     * from the right, same rationale as sparkline_render(). */
    for (int r = 0; r < n_rows; r++) {
        for (int i = 0; i < pad && pos[r] + 1 < out_cap; i++) {
            out_rows[r][pos[r]++] = ' ';
        }
    }

    for (int i = 0; i < n; i++) {
        int idx = (start + i) % SPARKLINE_HISTORY;
        double v = h->values[idx];
        int level; /* 0..total_levels, "how many eighth-rows are filled
                       from the bottom up" */
        if (range <= 0.0) {
            level = total_levels / 2; /* flat series: mid-height, not empty */
        } else {
            level = (int)(((v - lo) / range) * total_levels + 0.5);
            if (level < 0) level = 0;
            if (level > total_levels) level = total_levels;
        }

        /* Distribute `level` eighths across rows bottom-up. Row index r
         * (0 = top row as stored in out_rows) corresponds to
         * bottom-up rank (n_rows - 1 - r); that row's own 8-level
         * window covers [rank*8, rank*8 + 8) of the total scale. */
        for (int r = 0; r < n_rows; r++) {
            int rank_from_bottom = n_rows - 1 - r;
            int row_floor = rank_from_bottom * 8;
            int row_ceiling = row_floor + 8;
            const char *glyph;
            if (level <= row_floor) {
                glyph = " "; /* fully empty at this row -- plain space,
                                 1 byte, not a 3-byte UTF-8 glyph */
            } else if (level >= row_ceiling) {
                glyph = LEVELS[7]; /* fully filled: full block */
            } else {
                int sub_level = level - row_floor; /* 1..7 here */
                glyph = LEVELS[sub_level - 1];
            }
            size_t glyph_len = (glyph[0] == ' ') ? 1 : 3;
            if (pos[r] + glyph_len >= out_cap) continue; /* row buffer full; skip further cols for this row only */
            memcpy(out_rows[r] + pos[r], glyph, glyph_len);
            pos[r] += glyph_len;
        }
    }

    for (int r = 0; r < n_rows; r++) out_rows[r][pos[r]] = '\0';
}
