/*
 * out.h - streaming output sink for dictgen
 *
 * Supports writing to:
 *   - a plain file            (default)
 *   - a tar archive            (.tar)
 *   - a gzip-compressed tar    (.tar.gz / .tgz)
 *   - standard output          (forced with --stdout / -t / -tff)
 *
 * The sink streams data, so arbitrarily large dictionaries can be written
 * without buffering the whole thing in memory.  For .tar the size field is
 * patched in place once the stream ends (requires a seekable file); for
 * .tar.gz the payload is staged in a temporary file next to the archive and
 * then compressed in one streaming pass.
 */
#ifndef DICTGEN_OUT_H
#define DICTGEN_OUT_H

#include <stddef.h>

typedef struct Out Out;

/* Open an output sink.  If to_stdout is non-zero the path is ignored and
 * data is streamed to stdout as a plain byte stream.  Otherwise the path's
 * extension selects the container (.tar, .tar.gz, .tgz → tar / tar.gz).
 * Returns NULL on failure (errno set). */
Out *out_open(const char *path, int to_stdout);

/* Number of payload bytes written so far. */
unsigned long long out_written(const Out *o);

/* Append bytes.  Returns 0 on success, -1 on error. */
int out_write(Out *o, const void *buf, size_t n);

/* Append a NUL-terminated string. */
int out_puts(Out *o, const char *s);

/* Append formatted output (printf-style).  Handy for generators. */
int out_printf(Out *o, const char *fmt, ...);

/* Finish the stream: patch tar header / flush gzip.  Frees the sink. */
void out_close(Out *o);

/* Non-zero if a write error occurred. */
int out_error(const Out *o);

#endif /* DICTGEN_OUT_H */
