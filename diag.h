/* diag.h — SNOVA* diagnostics.
 *
 * Codes are stable identifiers, rendered as `SNOVA0001`. The lexer owns the
 * 0000-0099 range; later phases take their own bands so a code never moves.
 */
#ifndef SNOVAC_DIAG_H
#define SNOVAC_DIAG_H

#include <stdio.h>

#include "token.h"

typedef enum {
    SN_DIAG_ERROR = 0,
    SN_DIAG_WARNING
} SnDiagLevel;

/* Lexer band: 0001-0099 */
#define SNOVA_UNTERMINATED_STRING   1
#define SNOVA_UNTERMINATED_CHAR     2
#define SNOVA_UNKNOWN_CHARACTER     3
#define SNOVA_INVALID_NUMBER        4
#define SNOVA_INVALID_ESCAPE        5
#define SNOVA_UNTERMINATED_INTERP   6

/* The file a diagnostic's span is measured against. A single compilation
 * spans many files (the package graph pulls in every builtin), so the sink's
 * current file has to follow whichever file the phase is working on — one
 * fixed path/src would print every diagnostic under the entry file's name and
 * quote the wrong source line. */
typedef struct {
    const char *path;
    const char *src;
    size_t src_len;
} SnDiagFile;

typedef struct {
    SnDiagFile file;
    int error_count;
    int warning_count;
    int use_color;
    /* While non-zero, sn_diag_emit() drops the diagnostic and does not count
     * it. For phases that must resolve a declaration belonging to some other
     * context — a callee's signature at a call site — where anything wrong is
     * that declaration's own to report, in its own file and scope. */
    int quiet;
    FILE *out;
} SnDiagSink;

void sn_diag_init(SnDiagSink *d, const char *path, const char *src, size_t len);

/* Points the sink at `file` for every subsequent diagnostic and returns the
 * previous one, so a caller can restore it with a second call. `path` and
 * `src` must outlive every diagnostic emitted against them (arena- or
 * caller-owned; the sink does not copy). */
SnDiagFile sn_diag_set_file(SnDiagSink *d, SnDiagFile file);

void sn_diag_emit(SnDiagSink *d, SnDiagLevel level, int code, SnSpan span,
                  const char *fmt, ...);

#endif /* SNOVAC_DIAG_H */
