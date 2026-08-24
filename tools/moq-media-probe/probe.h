/*
 * moq_media_probe -- a small black-box probe over LibMoQ's MSF/CMSF catalog
 * parser, spoken as a deterministic one-request-per-line / one-response-per-line
 * JSON protocol on stdin/stdout.
 *
 * This header exposes the pure request handler so it can be unit-tested without
 * spawning a process. The handler owns no global state: each call parses one
 * request line, runs the requested operation through LibMoQ's existing parser,
 * and returns one response line. A failed request never affects a later one.
 *
 * The tool knows nothing about any external caller, corpus, or schema. It only
 * exposes LibMoQ's own model.
 */
#ifndef MOQ_MEDIA_PROBE_H
#define MOQ_MEDIA_PROBE_H

#include <moq/types.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOQ_MEDIA_PROBE_PROTOCOL "moq-media-probe/1"

/* Maximum accepted request-line length in bytes. A longer line is answered with
 * a typed `oversized-input` error rather than parsed. */
#define MOQ_MEDIA_PROBE_MAX_LINE ((size_t)(1u << 20)) /* 1 MiB */

/*
 * Handle exactly one request line and produce exactly one response line.
 *
 * `line`/`len` are the raw request bytes (a single JSONL record, without its
 * terminating newline; it need not be NUL-terminated). The response is a single
 * line of canonical JSON with NO trailing newline, returned NUL-terminated in a
 * freshly malloc()'d buffer via *out; the caller frees it with free().
 *
 * Returns MOQ_OK whenever a response was produced -- INCLUDING protocol/parse
 * errors, which are reported inside the response as {"status":"error",...}.
 * Returns MOQ_ERR_NOMEM only if no response could be allocated at all (in which
 * case *out is set to NULL). `alloc` backs the catalog parser; the response
 * buffer itself uses malloc/free.
 */
moq_result_t moq_media_probe_handle(const moq_alloc_t *alloc,
                                    const char *line, size_t len, char **out);

/*
 * Run the stdin -> stdout line loop until EOF. Reads bounded lines from `in`,
 * writes one response line (plus '\n') per request to `out`. Machine output
 * goes only to `out`; nothing is written to stderr on the normal path. Returns
 * 0 on clean EOF, non-zero only on an unrecoverable I/O/allocation failure
 * (input read error, short write, flush failure, or response-allocation OOM).
 */
int moq_media_probe_run(const moq_alloc_t *alloc, FILE *in, FILE *out);

/*
 * Injectable I/O seam behind moq_media_probe_run. Exposed so tests can drive the
 * loop with failing read/write/flush without a real failing stream. All output
 * flows through `write`/`flush`; input through `get`/`in_error`.
 */
typedef struct moq_media_probe_io {
    int  (*get)(void *ctx);                            /* next input byte [0,255], or <0 at EOF */
    bool (*in_error)(void *ctx);                       /* true if the input stream is in error */
    bool (*write)(void *ctx, const char *s, size_t n); /* write n bytes; false on failure */
    bool (*flush)(void *ctx);                          /* flush; false on failure */
    void *ctx;
} moq_media_probe_io_t;

int moq_media_probe_run_io(const moq_alloc_t *alloc, const moq_media_probe_io_t *io);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_MEDIA_PROBE_H */
