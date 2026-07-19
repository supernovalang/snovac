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

typedef struct {
    const char *path;
    const char *src;
    size_t src_len;
    int error_count;
    int warning_count;
    int use_color;
    FILE *out;
} SnDiagSink;

void sn_diag_init(SnDiagSink *d, const char *path, const char *src, size_t len);
void sn_diag_emit(SnDiagSink *d, SnDiagLevel level, int code, SnSpan span,
                  const char *fmt, ...);

#endif /* SNOVAC_DIAG_H */
