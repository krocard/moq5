/*
 * Negotiated-profile substrate: the FAIL-CLOSED CORPUS READER.
 *
 * One text corpus, three readers (C here, C++ reusing this one, Swift
 * independent), one FNV-1a 64 digest they must all agree on. The grammar is
 * deliberately narrow so it can be implemented identically in all three:
 *
 *   line 1        "np-corpus 1"
 *   line 2        "count <decimal N>"
 *   lines 3..N+2  "<transport> <media> <property> <value> <bytes>"
 *   line N+3      "end"
 *
 * and nothing else -- no comments, no blank lines, no trailing record, no
 * trailing garbage. Every field is a closed token set or a canonical form:
 *
 *   transport  d16 | d18
 *   media      loc01
 *   property   timestamp | type_delta | even_t2 | even_t4 | even_t6 |
 *              odd_prop | odd_hdr | after_odd | desync_2prop
 *                                            (see the record-kind table below)
 *   value      canonical decimal uint64: no sign, no leading zero unless "0"
 *   bytes      lowercase hex, even length, at least one byte, AT MOST
 *              NP_CORPUS_MAX_BYTES (64) decoded bytes -- a declared cap, part
 *              of the grammar, enforced identically by every reader
 *
 * Every closed token is matched as a COMPLETE SPAN, so a field containing an
 * embedded NUL is rejected rather than silently truncated to a legal token.
 *
 * RECORD KINDS -- what `value` means, and whether `bytes` is a whole property:
 *
 *   timestamp     a bare encoded integer; value is that integer
 *   type_delta    a bare encoded Delta Type field; value is the delta
 *   even_t2       a COMPLETE key-value pair whose absolute Type is 2
 *                 (LOC-01 Capture Timestamp); value is the integer value
 *   even_t4       the same, absolute Type 4 (LOC-01 Video Frame Marking)
 *   even_t6       the same, absolute Type 6 (LOC-01 Audio Level)
 *
 *                 The Type is named by the TOKEN, never inferred from the
 *                 bytes. An earlier grammar had one `even_prop` kind and let
 *                 the re-derivation try Types 2, 4 and 6 until one matched the
 *                 recorded bytes -- which made the bytes under test the
 *                 authority for their own semantic Type, so swapping a valid
 *                 Type-2 prefix for a valid Type-4 prefix authorized itself.
 *   odd_prop      a COMPLETE key-value pair with an odd absolute Type;
 *                 value is the payload LENGTH
 *   odd_hdr       the HEADER ONLY of an odd property (Delta Type + Length,
 *                 NO value bytes); value is the declared payload length. This
 *                 kind exists so a 16383/16384-byte Length can be carried in a
 *                 bounded record without materializing the payload -- and it
 *                 says so, rather than pretending to be a whole property.
 *   after_odd     a complete property whose PREVIOUS Type was odd, so the
 *                 delta parity and the Type parity disagree; value is the
 *                 integer value
 *   desync_2prop  two consecutive complete even properties; value is the FIRST
 *                 property's integer value
 *
 * `(transport, media, property, value)` is the semantic key and must be
 * unique. RELATION IS NOT A COLUMN: whether a pair of drafts reuses or
 * diverges is DERIVED by comparing the literal byte strings of rows that share
 * (media, property, value), so the corpus cannot assert a relation it does not
 * exhibit.
 */
#ifndef NP_CORPUS_H
#define NP_CORPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NP_CORPUS_MAX_RECORDS 128
#define NP_CORPUS_MAX_BYTES   64   /* decoded bytes per record */

typedef struct {
    char     transport[8];
    char     media[8];
    char     property[24];
    uint64_t value;
    uint8_t  bytes[NP_CORPUS_MAX_BYTES];
    size_t   n_bytes;
} np_corpus_rec_t;

typedef struct {
    np_corpus_rec_t recs[NP_CORPUS_MAX_RECORDS];
    size_t          n;
    uint64_t        digest;      /* FNV-1a 64 over the WHOLE file */
    size_t          file_len;
} np_corpus_t;

/* FNV-1a 64. Exposed so each reader can be pinned to the same known answers. */
#define NP_FNV1A64_OFFSET UINT64_C(14695981039346656037)
#define NP_FNV1A64_PRIME  UINT64_C(1099511628211)
uint64_t np_fnv1a64(const void *data, size_t len);

/*
 * Load and fully validate the corpus. Returns 0 on success, or a negative
 * error; on any error the corpus is REJECTED whole -- there is no partial
 * accept. `why` receives a short reason when non-NULL.
 */
int np_corpus_load(const char *path, np_corpus_t *out,
                   const char **why);

/* Parse from memory, same rules -- used by the grammar self-checks. */
int np_corpus_parse(const char *text, size_t len, np_corpus_t *out,
                    const char **why);

#endif /* NP_CORPUS_H */
