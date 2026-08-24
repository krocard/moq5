#include "np_corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t np_fnv1a64(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = NP_FNV1A64_OFFSET;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= NP_FNV1A64_PRIME;
    }
    return h;
}

/* ---- closed token sets, compared as COMPLETE SPANS --------------------- *
 * Never as C strings: a field such as "d16\0x" would compare equal to "d16"
 * under strcmp because the comparison stops at the embedded NUL, and the
 * independent Swift reader -- which keeps the byte -- would then reject a file
 * this reader accepted. The two readers must agree on the same bytes, so the
 * whole span is compared, length included. */

static bool span_is(const char *p, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len == n && memcmp(p, lit, n) == 0;
}

static bool tok_transport(const char *p, size_t len)
{
    return span_is(p, len, "d16") || span_is(p, len, "d18");
}
static bool tok_media(const char *p, size_t len)
{
    return span_is(p, len, "loc01");
}
static bool tok_property(const char *p, size_t len)
{
    static const char *const kProps[] = {
        "timestamp", "type_delta",
        "even_t2", "even_t4", "even_t6",     /* the Type is in the TOKEN */
        "odd_prop", "odd_hdr", "after_odd", "desync_2prop",
    };
    for (size_t i = 0; i < sizeof(kProps) / sizeof(kProps[0]); i++)
        if (span_is(p, len, kProps[i])) return true;
    return false;
}

/* ---- canonical field forms --------------------------------------------- */

/* Canonical decimal: at least one digit, no leading zero unless the whole
 * field is "0", and no overflow. */
static bool parse_u64_canonical(const char *s, size_t len, uint64_t *out)
{
    if (len == 0 || len > 20) return false;
    if (s[0] == '0' && len != 1) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return false;   /* overflow */
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* Canonical hex: lowercase only, even length, at least one byte. */
static bool parse_hex_canonical(const char *s, size_t len,
                                uint8_t *out, size_t cap, size_t *n_out)
{
    if (len == 0 || (len % 2) != 0) return false;
    /* the DECLARED per-record byte cap, part of the grammar and enforced
     * identically by the Swift reader */
    if (len / 2 > NP_CORPUS_MAX_BYTES) return false;
    if (len / 2 > cap) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;              /* uppercase is NOT canonical */
    }
    for (size_t i = 0; i < len; i += 2) {
        unsigned hi = (unsigned)(s[i] <= '9' ? s[i] - '0' : s[i] - 'a' + 10);
        unsigned lo = (unsigned)(s[i+1] <= '9' ? s[i+1] - '0' : s[i+1] - 'a' + 10);
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *n_out = len / 2;
    return true;
}

/* ---- line splitting ---------------------------------------------------- */

typedef struct { const char *p; size_t len; } np_span_t;

/* Exactly `want` single-space-separated fields, no leading/trailing space, no
 * empty field, no tab, no double space. */
static bool split_fields(const char *line, size_t len, np_span_t *f, size_t want)
{
    size_t got = 0, start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == ' ') {
            if (i == start) return false;            /* empty field */
            if (got >= want) return false;           /* too many */
            f[got].p = line + start;
            f[got].len = i - start;
            got++;
            start = i + 1;
        } else if (line[i] == '\t' || line[i] == '\r') {
            return false;
        }
    }
    return got == want;
}

static bool span_eq(np_span_t s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.p, lit, n) == 0;
}

static bool span_copy(np_span_t s, char *dst, size_t cap)
{
    if (s.len + 1 > cap) return false;
    memcpy(dst, s.p, s.len);
    dst[s.len] = '\0';
    return true;
}

#define REJECT(msg) do { if (why) *why = (msg); return -1; } while (0)

int np_corpus_parse(const char *text, size_t len, np_corpus_t *out,
                    const char **why)
{
    if (!text || !out) REJECT("null argument");
    memset(out, 0, sizeof(*out));
    out->digest = np_fnv1a64(text, len);
    out->file_len = len;

    /* The file must be a whole number of newline-terminated lines: a missing
     * final newline is truncation, not a lenient last record. */
    if (len == 0 || text[len - 1] != '\n') REJECT("no final newline");

    np_span_t lines[NP_CORPUS_MAX_RECORDS + 8];
    size_t n_lines = 0, start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] != '\n') continue;
        if (n_lines >= sizeof(lines) / sizeof(lines[0]))
            REJECT("too many lines");
        lines[n_lines].p = text + start;
        lines[n_lines].len = i - start;
        n_lines++;
        start = i + 1;
    }
    if (n_lines < 3) REJECT("too few lines");

    /* header */
    if (!span_eq(lines[0], "np-corpus 1")) REJECT("bad magic/version");
    np_span_t hf[2];
    if (!split_fields(lines[1].p, lines[1].len, hf, 2)) REJECT("bad count line");
    if (!span_eq(hf[0], "count")) REJECT("bad count keyword");
    uint64_t declared = 0;
    if (!parse_u64_canonical(hf[1].p, hf[1].len, &declared))
        REJECT("bad count value");
    if (declared == 0 || declared > NP_CORPUS_MAX_RECORDS)
        REJECT("count out of range");

    /* exactly declared records, then exactly one terminator, then nothing */
    if (n_lines != declared + 3) REJECT("line count disagrees with count");
    if (!span_eq(lines[n_lines - 1], "end")) REJECT("missing terminator");

    for (size_t r = 0; r < declared; r++) {
        np_span_t f[5];
        const np_span_t *ln = &lines[2 + r];
        if (!split_fields(ln->p, ln->len, f, 5)) REJECT("bad record arity");
        np_corpus_rec_t *rec = &out->recs[r];
        /* the SPAN is validated before anything is copied, so an embedded NUL
         * cannot be truncated away into a legal token */
        if (!tok_transport(f[0].p, f[0].len)) REJECT("unknown transport token");
        if (!tok_media(f[1].p, f[1].len))     REJECT("unknown media token");
        if (!tok_property(f[2].p, f[2].len))  REJECT("unknown property token");
        if (!span_copy(f[0], rec->transport, sizeof(rec->transport)))
            REJECT("transport too long");
        if (!span_copy(f[1], rec->media, sizeof(rec->media)))
            REJECT("media too long");
        if (!span_copy(f[2], rec->property, sizeof(rec->property)))
            REJECT("property too long");
        if (!parse_u64_canonical(f[3].p, f[3].len, &rec->value))
            REJECT("non-canonical value");
        if (!parse_hex_canonical(f[4].p, f[4].len, rec->bytes,
                                 sizeof(rec->bytes), &rec->n_bytes))
            REJECT("non-canonical bytes");
        out->n = r + 1;
    }

    /* the semantic key is unique */
    for (size_t i = 0; i < out->n; i++)
        for (size_t j = i + 1; j < out->n; j++) {
            const np_corpus_rec_t *a = &out->recs[i], *b = &out->recs[j];
            if (strcmp(a->transport, b->transport) == 0 &&
                strcmp(a->media, b->media) == 0 &&
                strcmp(a->property, b->property) == 0 &&
                a->value == b->value)
                REJECT("duplicate semantic key");
        }

    if (why) *why = NULL;
    return 0;
}

int np_corpus_load(const char *path, np_corpus_t *out, const char **why)
{
    if (!path || !out) REJECT("null argument");
    FILE *f = fopen(path, "rb");
    if (!f) REJECT("cannot open corpus");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); REJECT("seek failed"); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); REJECT("tell failed"); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); REJECT("rewind failed"); }
    if (sz > 1 << 20) { fclose(f); REJECT("corpus implausibly large"); }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); REJECT("out of memory"); }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); REJECT("short read"); }
    buf[sz] = '\0';
    int rc = np_corpus_parse(buf, (size_t)sz, out, why);
    free(buf);
    return rc;
}
