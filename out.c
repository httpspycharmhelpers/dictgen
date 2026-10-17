/*
 * out.c - streaming output sink (see out.h)
 */
#define _GNU_SOURCE
#include "out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <zlib.h>

#define OUT_BUFSZ (4u * 1024u * 1024u)
#define TAR_BLOCK 512

enum { M_STDOUT, M_FILE, M_TAR, M_TARGZ };

struct Out {
    int      mode;
    int      err;
    FILE    *fp;          /* active stream (plain file / temp / gz / stdout) */
    char    *path;        /* destination path (owned) */
    char    *tmp;         /* staged payload path for tgz (owned) */
    char     name[100];   /* tar entry name */
    long     hdr_pos;     /* file offset of the tar header (M_TAR) */
    unsigned long long written;
    char    *buf;         /* write buffer */
    size_t   buflen;
};

/* ---- tar helpers ------------------------------------------------------- */

static void tar_octal(char *dst, size_t size, unsigned long long v)
{
    /* size includes the terminating NUL; write zero-padded octal */
    memset(dst, '0', size);
    dst[size - 1] = '\0';
    int i = (int)size - 2;
    while (v > 0 && i >= 0) {
        dst[i--] = (char)('0' + (v & 7));
        v >>= 3;
    }
}

static void tar_base256(char *dst, size_t size, unsigned long long v)
{
    /* GNU base-256 encoding for values that do not fit in octal */
    memset(dst, 0, size);
    dst[0] = (char)0x80;
    for (size_t i = 0; i < size - 1; i++) {
        dst[size - 1 - i] = (char)(v & 0xff);
        v >>= 8;
    }
}

/* Fill a 512-byte tar header block. */
static void tar_header(unsigned char *h, const char *name, unsigned long long size,
                       unsigned long long mtime)
{
    memset(h, 0, TAR_BLOCK);
    strncpy((char *)h, name, 99);
    tar_octal((char *)h + 100, 8, 0644);           /* mode */
    tar_octal((char *)h + 108, 8, 0);              /* uid  */
    tar_octal((char *)h + 116, 8, 0);              /* gid  */
    if (size < 010000000000ULL)                    /* fits in 11 octal digits */
        tar_octal((char *)h + 124, 12, size);
    else
        tar_base256((char *)h + 124, 12, size);
    tar_octal((char *)h + 136, 12, mtime);
    memset(h + 148, ' ', 8);                       /* chksum placeholder */
    h[156] = '0';                                  /* typeflag: regular file */
    memcpy(h + 257, "ustar", 5);
    h[263] = '0'; h[264] = '0';
    strncpy((char *)h + 265, "root", 31);
    strncpy((char *)h + 297, "root", 31);
    unsigned sum = 0;
    for (int i = 0; i < TAR_BLOCK; i++) sum += h[i];
    /* checksum: six octal digits, NUL, space (7-byte field + trailing space) */
    char ck[7];
    tar_octal(ck, sizeof(ck), sum);
    memcpy(h + 148, ck, sizeof(ck));
    h[155] = ' ';
}

/* Derive a tar entry name (file name inside the archive) from the path. */
static void derive_entry_name(const char *path, char *out, size_t outsz)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    /* strip known container suffixes */
    const char *suf[] = { ".tar.gz", ".tgz", ".tar", NULL };
    for (int i = 0; suf[i]; i++) {
        size_t sl = strlen(suf[i]);
        if (n > sl && strcmp(base + n - sl, suf[i]) == 0) {
            n -= sl;
            break;
        }
    }
    if (n == 0) {
        snprintf(out, outsz, "output.txt");
        return;
    }
    if (n >= outsz) n = outsz - 1;
    memcpy(out, base, n);
    /* if the stem has no extension, add .txt */
    int has_dot = 0;
    for (size_t i = 0; i < n; i++) if (out[i] == '.') has_dot = 1;
    if (!has_dot && n + 4 < outsz) {
        memcpy(out + n, ".txt", 4);
        return;
    }
    out[n] = '\0';
}

static int ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    return strcmp(s + ls - lf, suf) == 0;
}

/* ---- buffered layer ---------------------------------------------------- */

static int raw_write(Out *o, const void *buf, size_t n)
{
    if (fwrite(buf, 1, n, o->fp) != n) {
        o->err = 1;
        return -1;
    }
    return 0;
}

static int flush_buf(Out *o)
{
    if (o->buflen == 0) return 0;
    if (raw_write(o, o->buf, o->buflen) != 0) return -1;
    o->buflen = 0;
    return 0;
}

int out_write(Out *o, const void *buf, size_t n)
{
    const char *p = buf;
    size_t remaining = n;
    while (remaining > 0) {
        size_t space = OUT_BUFSZ - o->buflen;
        if (space == 0) {
            if (flush_buf(o) != 0) return -1;
            space = OUT_BUFSZ;
        }
        size_t chunk = remaining < space ? remaining : space;
        memcpy(o->buf + o->buflen, p, chunk);
        o->buflen += chunk;
        p += chunk;
        remaining -= chunk;
    }
    o->written += (unsigned long long)n;   /* logical payload bytes */
    return 0;
}

int out_puts(Out *o, const char *s) { return out_write(o, s, strlen(s)); }

int out_printf(Out *o, const char *fmt, ...)
{
    char stackbuf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);
    if (n < 0) { o->err = 1; return -1; }
    if ((size_t)n < sizeof(stackbuf))
        return out_write(o, stackbuf, (size_t)n);
    char *big = malloc((size_t)n + 1);
    if (!big) { o->err = 1; return -1; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = out_write(o, big, (size_t)n);
    free(big);
    return r;
}

unsigned long long out_written(const Out *o) { return o->written; }
int out_error(const Out *o) { return o->err; }

/* ---- open / close ------------------------------------------------------ */

Out *out_open(const char *path, int to_stdout)
{
    Out *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->buf = malloc(OUT_BUFSZ);
    if (!o->buf) { free(o); return NULL; }

    if (to_stdout) {
        o->mode = M_STDOUT;
        o->fp = stdout;
        return o;
    }

    o->path = strdup(path ? path : "-");
    if (!o->path) { free(o->buf); free(o); return NULL; }

    if (ends_with(path, ".tar.gz") || ends_with(path, ".tgz")) {
        o->mode = M_TARGZ;
        derive_entry_name(path, o->name, sizeof(o->name));
        /* stage payload in a temp file next to the archive */
        size_t tl = strlen(path) + 16;
        o->tmp = malloc(tl);
        if (!o->tmp) goto fail;
        snprintf(o->tmp, tl, "%s.tmpXXXXXX", path);
        int fd = mkstemp(o->tmp);
        if (fd < 0) goto fail;
        o->fp = fdopen(fd, "wb+");
        if (!o->fp) { close(fd); goto fail; }
    } else if (ends_with(path, ".tar")) {
        o->mode = M_TAR;
        derive_entry_name(path, o->name, sizeof(o->name));
        o->fp = fopen(path, "wb+");
        if (!o->fp) goto fail;
        unsigned char h[TAR_BLOCK];
        tar_header(h, o->name, 0, 0);
        o->hdr_pos = ftell(o->fp);
        if (raw_write(o, h, TAR_BLOCK) != 0) goto fail;
    } else {
        o->mode = M_FILE;
        o->fp = fopen(path, "wb");
        if (!o->fp) goto fail;
    }
    return o;

fail:
    {
    int e = errno;
    if (o->fp && o->mode != M_STDOUT) fclose(o->fp);
    if (o->tmp) unlink(o->tmp);
    free(o->tmp); free(o->path); free(o->buf); free(o);
    errno = e;
    return NULL;
    }
}

void out_close(Out *o)
{
    if (!o) return;

    flush_buf(o);

    if (o->mode == M_TAR) {
        /* pad payload to 512 and patch the header */
        unsigned long long rem = o->written % TAR_BLOCK;
        if (rem) {
            unsigned char z[TAR_BLOCK];
            memset(z, 0, sizeof(z));
            raw_write(o, z, TAR_BLOCK - rem);
        }
        /* two zero blocks terminate the archive */
        unsigned char z[TAR_BLOCK * 2];
        memset(z, 0, sizeof(z));
        long end = ftell(o->fp);
        raw_write(o, z, sizeof(z));
        /* rewrite header with real size */
        unsigned char h[TAR_BLOCK];
        tar_header(h, o->name, o->written, 0);
        if (fseek(o->fp, o->hdr_pos, SEEK_SET) == 0) {
            raw_write(o, h, TAR_BLOCK);
            fseek(o->fp, end, SEEK_SET);
        }
        fclose(o->fp);
    } else if (o->mode == M_TARGZ) {
        fflush(o->fp);
        fclose(o->fp);          /* close temp */
        /* now build the gzip archive from the staged payload */
        gzFile gz = gzopen(o->path, "wb6");
        if (gz) {
            unsigned char h[TAR_BLOCK];
            tar_header(h, o->name, o->written, 0);
            gzwrite(gz, h, TAR_BLOCK);
            FILE *in = fopen(o->tmp, "rb");
            if (in) {
                char *cb = malloc(1u << 20);
                size_t r;
                while (cb && (r = fread(cb, 1, 1u << 20, in)) > 0)
                    gzwrite(gz, cb, (unsigned)r);
                free(cb);
                unsigned long long rem = o->written % TAR_BLOCK;
                if (rem) {
                    unsigned char z[TAR_BLOCK];
                    memset(z, 0, sizeof(z));
                    gzwrite(gz, z, (unsigned)(TAR_BLOCK - rem));
                }
                unsigned char z2[TAR_BLOCK * 2];
                memset(z2, 0, sizeof(z2));
                gzwrite(gz, z2, (unsigned)sizeof(z2));
                fclose(in);
            } else {
                o->err = 1;
            }
            gzclose(gz);
        } else {
            o->err = 1;
        }
        unlink(o->tmp);
    } else if (o->mode == M_FILE) {
        fclose(o->fp);
    } else { /* M_STDOUT */
        fflush(o->fp);
    }

    free(o->tmp);
    free(o->path);
    free(o->buf);
    free(o);
}
