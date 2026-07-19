#include "diag.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define sn_isatty(fd) 0
#else
#include <unistd.h>
#define sn_isatty(fd) isatty(fd)
#endif

void sn_diag_init(SnDiagSink *d, const char *path, const char *src, size_t len) {
    d->path = path;
    d->src = src;
    d->src_len = len;
    d->error_count = 0;
    d->warning_count = 0;
    d->out = stderr;
    d->use_color = sn_isatty(2) && !getenv("NO_COLOR");
}

/* Returns the [start,end) byte range of the 1-based line containing `offset`. */
static void line_bounds(const SnDiagSink *d, uint32_t offset,
                        size_t *start, size_t *end) {
    size_t s = offset <= d->src_len ? offset : d->src_len;
    while (s > 0 && d->src[s - 1] != '\n') {
        s--;
    }
    size_t e = s;
    while (e < d->src_len && d->src[e] != '\n') {
        e++;
    }
    *start = s;
    *end = e;
}

void sn_diag_emit(SnDiagSink *d, SnDiagLevel level, int code, SnSpan span,
                  const char *fmt, ...) {
    const char *label = (level == SN_DIAG_ERROR) ? "error" : "warning";
    const char *col_lvl = "";
    const char *col_dim = "";
    const char *col_off = "";
    if (d->use_color) {
        col_lvl = (level == SN_DIAG_ERROR) ? "\033[1;31m" : "\033[1;33m";
        col_dim = "\033[2m";
        col_off = "\033[0m";
    }

    if (level == SN_DIAG_ERROR) {
        d->error_count++;
    } else {
        d->warning_count++;
    }

    fprintf(d->out, "%s%s[SNOVA%04d]%s: ", col_lvl, label, code, col_off);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(d->out, fmt, ap);
    va_end(ap);

    fprintf(d->out, "\n %s--> %s:%u:%u%s\n", col_dim, d->path, span.line,
            span.col, col_off);

    size_t ls, le;
    line_bounds(d, span.offset, &ls, &le);
    if (le > ls) {
        fprintf(d->out, "  %s|%s %.*s\n", col_dim, col_off, (int)(le - ls),
                d->src + ls);
        fprintf(d->out, "  %s|%s ", col_dim, col_off);
        for (size_t i = ls; i < span.offset && i < le; i++) {
            fputc(d->src[i] == '\t' ? '\t' : ' ', d->out);
        }
        uint32_t width = span.len ? span.len : 1u;
        if (span.offset + width > le) {
            width = (uint32_t)(le - span.offset);
            if (width == 0) {
                width = 1;
            }
        }
        fprintf(d->out, "%s", col_lvl);
        for (uint32_t i = 0; i < width; i++) {
            fputc('^', d->out);
        }
        fprintf(d->out, "%s\n", col_off);
    }
}
