/*
 * Negotiated-profile substrate, ROLE 1 of 4: the PURE INDEPENDENT ORACLE.
 *
 * This role re-derives the two variable-length integer encodings the drafts
 * specify, from the drafts, using the C standard library ONLY. It must never
 * include or reference a moq_* product API: the whole point is that when the
 * product and the oracle disagree, the disagreement is evidence rather than a
 * shared bug. That is enforced outside this file, by
 * tests/cmake/check_np_roles.cmake, which scans both this SOURCE for moq_
 * tokens and its compiled OBJECT for moq_ symbols.
 *
 *   draft-ietf-moq-transport-16 §1.4  -- fields marked (i) use the RFC 9000
 *     QUIC variable-length integer: a 2-bit length prefix selecting 1/2/4/8
 *     bytes, maximum 2^62-1.
 *   draft-ietf-moq-transport-18 §1.4.1 -- fields marked (vi64) use MOQT's own
 *     encoding: the count of leading 1 bits of the first byte selects 1..9
 *     bytes, maximum 2^64-1.
 *
 * The two agree byte-for-byte only on 0..63. From 64 they diverge, and 64..127
 * is the QUIET band -- quiet, but NOT value-preserving. QUIC encodes that band
 * as two bytes, 0x40|(v>>8) then v&0xFF, whose first byte is always 0x40; a
 * vi64 reader consumes exactly ONE of those two bytes and reports its low 7
 * bits, i.e. 64, for every value in the band. So:
 *
 *   - the read never errors and the value is in range;
 *   - the stream desynchronizes by one byte;
 *   - the VALUE survives only at v == 64 and is silently wrong (reported as
 *     64) for 65..127.
 *
 * Either way a value-level oracle sees nothing, which is the point.
 */
#ifndef NP_ORACLE_H
#define NP_ORACLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NP_ORACLE_MAX_ENC 9   /* vi64's longest form */

/* The largest value each encoding can represent. */
#define NP_QUIC_VARINT_MAX UINT64_C(4611686018427387903)   /* 2^62 - 1 */
#define NP_VI64_MAX        UINT64_C(18446744073709551615)  /* 2^64 - 1 */

typedef enum {
    NP_ENC_QUIC_VARINT = 1,   /* draft-16 (i)    */
    NP_ENC_VI64        = 2,   /* draft-18 (vi64) */
} np_enc_t;

/*
 * Encode v in its MINIMAL form. Returns the byte count, or 0 if the encoding
 * cannot represent v (only possible for QUIC varint above 2^62-1). out must
 * hold NP_ORACLE_MAX_ENC bytes.
 */
size_t np_encode(np_enc_t enc, uint64_t v, uint8_t *out);

/*
 * Decode one integer from buf[0..len). Returns the byte count consumed, or 0
 * on truncation / an unrepresentable form. *out receives the value.
 *
 * A short buffer is a decode failure, never an over-read: the length implied
 * by the first byte is checked against len before any further byte is touched.
 */
size_t np_decode(np_enc_t enc, const uint8_t *buf, size_t len, uint64_t *out);

/*
 * KEY-VALUE-PAIR framing, per draft-ietf-moq-transport-18 §1.4.3 (and the
 * identical draft-16 structure with (i) in place of (vi64)):
 *
 *   Key-Value-Pair { Delta Type (vi64), [Length (vi64),] Value (..) }
 *
 * The drafts are precise about which parity decides the form, and it is NOT
 * the delta:
 *
 *   "Delta Type: ... identifying the Type as a delta encoded value from the
 *    previous Type, if any."
 *   "Length: Only present when TYPE is odd."
 *   "Value: A single varint encoded value when TYPE is even, otherwise a
 *    sequence of Length bytes."
 *
 * So the form follows the ABSOLUTE Type, and the wire carries `type -
 * previous_type`. Those two parities coincide only while the previous Type is
 * even, which is why this API takes both: an interface keyed on the delta
 * alone is right by accident and wrong as soon as a property follows an odd
 * one.
 *
 * All three encoders return the byte count, or 0 on any refusal:
 *   - a wrong value form for the Type's parity;
 *   - type < previous_type (Types are canonically NONDECREASING, since the
 *     delta is an unsigned integer);
 *   - a Length above the draft's 2^16-1 maximum;
 *   - an encoding that cannot represent the delta or the length;
 *   - a destination too small -- refused whole, never truncated.
 */
#define NP_KVP_MAX_VALUE_LEN 65535   /* draft: "maximum length ... 2^16-1" */

/* EVEN absolute type: Delta Type then a single encoded integer, no Length. */
size_t np_encode_prop_int(np_enc_t enc, uint64_t previous_type, uint64_t type,
                          uint64_t value, uint8_t *out, size_t cap);

/* ODD absolute type: Delta Type, Length, then `val_len` value bytes. */
size_t np_encode_prop_bytes(np_enc_t enc, uint64_t previous_type,
                            uint64_t type, const uint8_t *val, size_t val_len,
                            uint8_t *out, size_t cap);

/*
 * The HEADER ONLY of an odd property: Delta Type and Length, with no value
 * bytes. This exists so a corpus can carry a large declared Length -- 16383 or
 * 16384 -- as a bounded record, without materializing a 16 KiB payload. A
 * record built this way is not a complete property and must say so.
 */
size_t np_encode_prop_bytes_header(np_enc_t enc, uint64_t previous_type,
                                   uint64_t type, uint64_t val_len,
                                   uint8_t *out, size_t cap);

/*
 * Run the oracle's own known-answer, boundary and malformed-input self-checks.
 * Returns the number of failures; 0 means the oracle may be trusted by the
 * rows above it. Quiet on success.
 */
int np_oracle_self_check(void);

#endif /* NP_ORACLE_H */
