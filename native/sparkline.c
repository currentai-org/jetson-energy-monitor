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
