#include "np_oracle.h"

#include <stdio.h>
#include <string.h>

/* ---- draft-16 (i): RFC 9000 QUIC variable-length integer --------------- *
 * The two most significant bits of the first byte select the length; the
 * remaining 62 bits carry the value in network byte order. */

static size_t quic_len_for(uint64_t v)
{
    if (v <= UINT64_C(63))         return 1;
    if (v <= UINT64_C(16383))      return 2;
    if (v <= UINT64_C(1073741823)) return 4;
    if (v <= NP_QUIC_VARINT_MAX)   return 8;
    return 0;                      /* not representable */
}

static size_t quic_encode(uint64_t v, uint8_t *out)
{
    size_t n = quic_len_for(v);
    if (n == 0) return 0;
    uint8_t prefix = (uint8_t)(n == 1 ? 0x00 : n == 2 ? 0x40 : n == 4 ? 0x80
                                                                     : 0xC0);
    for (size_t i = 0; i < n; i++)
        out[n - 1 - i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    out[0] = (uint8_t)(out[0] | prefix);
    return n;
}

static size_t quic_decode(const uint8_t *buf, size_t len, uint64_t *out)
{
    if (len == 0) return 0;
    size_t n = (size_t)1u << (buf[0] >> 6);
    if (len < n) return 0;                       /* checked BEFORE reading */
    uint64_t v = (uint64_t)(buf[0] & 0x3F);
    for (size_t i = 1; i < n; i++) v = (v << 8) | buf[i];
    *out = v;
    return n;
}

/* ---- draft-18 (vi64): leading-1-bit unary length prefix ---------------- *
 * The number of leading 1 bits of the first byte gives the length in bytes,
 * 1..9. For lengths 1..8 the value continues in the bits after the first 0;
 * for length 9 the first byte is 0xFF and all 64 value bits follow. */

static size_t vi64_len_for(uint64_t v)
{
    if (v <= UINT64_C(127))                  return 1;   /* 7 usable bits  */
    if (v <= UINT64_C(16383))                return 2;   /* 14             */
    if (v <= UINT64_C(2097151))              return 3;   /* 21             */
    if (v <= UINT64_C(268435455))            return 4;   /* 28             */
    if (v <= UINT64_C(34359738367))          return 5;   /* 35             */
    if (v <= UINT64_C(4398046511103))        return 6;   /* 42             */
    if (v <= UINT64_C(562949953421311))      return 7;   /* 49             */
    if (v <= UINT64_C(72057594037927935))    return 8;   /* 56             */
    return 9;                                            /* 64             */
}

static size_t vi64_encode(uint64_t v, uint8_t *out)
{
    size_t n = vi64_len_for(v);
    if (n == 9) {
        out[0] = 0xFF;
        for (size_t i = 0; i < 8; i++)
            out[1 + i] = (uint8_t)((v >> (8 * (7 - i))) & 0xFF);
        return 9;
    }
    /* n leading 1 bits then a 0, i.e. the top n bits of a byte set except the
     * (n+1)-th; the remaining 8-n-1 bits of byte 0 are the value's high bits. */
    for (size_t i = 0; i < n; i++)
        out[n - 1 - i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    uint8_t prefix = (uint8_t)(0xFFu << (8 - (n - 1)));   /* n-1 leading 1s */
    out[0] = (uint8_t)(out[0] | prefix);
    return n;
}

static size_t vi64_decode(const uint8_t *buf, size_t len, uint64_t *out)
{
    if (len == 0) return 0;
    uint8_t b0 = buf[0];
    size_t lead = 0;
    while (lead < 8 && (b0 & (uint8_t)(0x80u >> lead))) lead++;
    size_t n = lead + 1;                          /* 1..9 */
    if (len < n) return 0;                        /* checked BEFORE reading */
    uint64_t v;
    if (n == 9) {
        v = 0;
        for (size_t i = 0; i < 8; i++) v = (v << 8) | buf[1 + i];
    } else {
        uint8_t mask = (uint8_t)(0xFFu >> n);     /* bits after the 0 marker */
        v = (uint64_t)(b0 & mask);
        for (size_t i = 1; i < n; i++) v = (v << 8) | buf[i];
    }
    *out = v;
    return n;
}

/* ---- dispatch ---------------------------------------------------------- */

size_t np_encode(np_enc_t enc, uint64_t v, uint8_t *out)
{
    /* An unknown codec is REFUSED, not silently treated as vi64: a caller that
     * passes a future draft's codec must fail loudly here rather than be given
     * the wrong encoding. */
    if (enc == NP_ENC_QUIC_VARINT) return quic_encode(v, out);
    if (enc == NP_ENC_VI64)        return vi64_encode(v, out);
    return 0;
}

size_t np_decode(np_enc_t enc, const uint8_t *buf, size_t len, uint64_t *out)
{
    if (enc == NP_ENC_QUIC_VARINT) return quic_decode(buf, len, out);
    if (enc == NP_ENC_VI64)        return vi64_decode(buf, len, out);
    return 0;
}

/*
 * TRANSACTIONAL by construction.
 *
 * Every one of the three public helpers assembles its small fields into a
 * LOCAL buffer, preflights the complete required length, and only then copies
 * to the caller's destination. An earlier revision emitted the Delta Type
 * straight into `out` and discovered the overflow afterwards, so a refused
 * call left a header behind -- and the self-check that was supposed to catch
 * that compared only the bytes neither failed call could have touched. A
 * refusal must leave the destination byte-for-byte unchanged, and now does.
 *
 * The header is at most two encoded integers, so the local buffer is 2 *
 * NP_ORACLE_MAX_ENC bytes; the value payload is never copied twice and never
 * materialized, so a 65535-byte Length costs nothing here.
 */
#define NP_PROP_HDR_MAX (2 * NP_ORACLE_MAX_ENC)

/*
 * Build a property header into `hdr`: the Delta Type, plus the Length when the
 * absolute Type is odd. Returns the header length, or 0 on refusal. Touches
 * nothing the caller owns.
 */
static size_t build_prop_header(np_enc_t enc, uint64_t previous_type,
                                uint64_t type, bool want_length,
                                uint64_t length, uint8_t *hdr)
{
    uint8_t tmp[NP_ORACLE_MAX_ENC];
    if (type < previous_type) return 0;        /* Types are nondecreasing */
    uint64_t delta = type - previous_type;     /* cannot underflow now */
    size_t n = np_encode(enc, delta, tmp);
    if (n == 0) return 0;
    memcpy(hdr, tmp, n);
    if (!want_length) return n;
    if (length > NP_KVP_MAX_VALUE_LEN) return 0;
    size_t n2 = np_encode(enc, length, tmp);
    if (n2 == 0) return 0;
    memcpy(hdr + n, tmp, n2);
    return n + n2;
}

size_t np_encode_prop_int(np_enc_t enc, uint64_t previous_type, uint64_t type,
                          uint64_t value, uint8_t *out, size_t cap)
{
    uint8_t buf[NP_PROP_HDR_MAX + NP_ORACLE_MAX_ENC];
    if ((type & 1u) != 0) return 0;            /* odd Type needs the byte form */
    size_t n1 = build_prop_header(enc, previous_type, type, false, 0, buf);
    if (n1 == 0) return 0;
    size_t n2 = np_encode(enc, value, buf + n1);
    if (n2 == 0) return 0;
    if (n1 + n2 > cap) return 0;               /* refused BEFORE any write */
    memcpy(out, buf, n1 + n2);
    return n1 + n2;
}

size_t np_encode_prop_bytes_header(np_enc_t enc, uint64_t previous_type,
                                   uint64_t type, uint64_t val_len,
                                   uint8_t *out, size_t cap)
{
    uint8_t buf[NP_PROP_HDR_MAX];
    if ((type & 1u) == 0) return 0;            /* even Type has no Length */
    size_t n = build_prop_header(enc, previous_type, type, true, val_len, buf);
    if (n == 0) return 0;
    if (n > cap) return 0;
    memcpy(out, buf, n);
    return n;
}

size_t np_encode_prop_bytes(np_enc_t enc, uint64_t previous_type,
                            uint64_t type, const uint8_t *val, size_t val_len,
                            uint8_t *out, size_t cap)
{
    uint8_t buf[NP_PROP_HDR_MAX];
    if ((type & 1u) == 0) return 0;
    if (val_len > 0 && val == NULL) return 0;  /* refused without writing */
    size_t n = build_prop_header(enc, previous_type, type,
                                 true, (uint64_t)val_len, buf);
    if (n == 0) return 0;
    if (val_len > SIZE_MAX - n) return 0;      /* total cannot overflow */
    size_t total = n + val_len;
    if (total > cap) return 0;                 /* refused BEFORE any write */
    memcpy(out, buf, n);
    if (val_len) memcpy(out + n, val, val_len);
    return total;
}

/* ---- self-check -------------------------------------------------------- */

static int oc_fail;
#define OC(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL: np_oracle %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    oc_fail++; } } while (0)

static bool enc_equals(np_enc_t e, uint64_t v, const char *hex)
{
    uint8_t buf[NP_ORACLE_MAX_ENC];
    size_t n = np_encode(e, v, buf);
    size_t want = strlen(hex) / 2;
    if (n != want) return false;
    for (size_t i = 0; i < n; i++) {
        char pair[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        unsigned byte = 0;
        if (sscanf(pair, "%02x", &byte) != 1) return false;
        if (buf[i] != (uint8_t)byte) return false;
    }
    return true;
}

static bool roundtrips(np_enc_t e, uint64_t v)
{
    uint8_t buf[NP_ORACLE_MAX_ENC];
    size_t n = np_encode(e, v, buf);
    if (n == 0) return false;
    uint64_t back = ~v;
    return np_decode(e, buf, n, &back) == n && back == v;
}

int np_oracle_self_check(void)
{
    oc_fail = 0;

    /* KNOWN ANSWERS, draft-18 Table 2 (the draft's own examples). */
    OC(enc_equals(NP_ENC_VI64, 37, "25"));
    OC(enc_equals(NP_ENC_VI64, 15293, "bbbd"));
    OC(enc_equals(NP_ENC_VI64, 226442877, "ed7f3e7d"));
    OC(enc_equals(NP_ENC_VI64, UINT64_C(2893212287960), "faa1a0e403d8"));
    OC(enc_equals(NP_ENC_VI64, UINT64_C(151288809941952), "fc8998abc66bc0"));
    OC(enc_equals(NP_ENC_VI64, UINT64_C(70423237261249041),
                  "fefa318fa8e3ca11"));
    OC(enc_equals(NP_ENC_VI64, NP_VI64_MAX, "ffffffffffffffffff"));

    /*
     * NON-MINIMAL forms decode too: draft-18 §1.4.1 says "Variable length
     * integers do not need to be encoded using the minimum number of bytes",
     * and its Table 2 lists 0x8025 as a second spelling of 37. The ENCODER is
     * minimal by choice; the DECODER must accept both.
     */
    {
        static const uint8_t kNonMinimal37[2] = { 0x80, 0x25 };
        uint64_t got = 0;
        OC(np_decode(NP_ENC_VI64, kNonMinimal37, 2, &got) == 2 && got == 37);
        OC(enc_equals(NP_ENC_VI64, 37, "25"));   /* we still emit the minimal */
    }

    /* KNOWN ANSWERS, RFC 9000 section 16 examples for the draft-16 (i) form. */
    OC(enc_equals(NP_ENC_QUIC_VARINT, 37, "25"));
    OC(enc_equals(NP_ENC_QUIC_VARINT, 15293, "7bbd"));
    OC(enc_equals(NP_ENC_QUIC_VARINT, 494878333, "9d7f3e7d"));
    OC(enc_equals(NP_ENC_QUIC_VARINT, UINT64_C(151288809941952652),
                  "c2197c5eff14e88c"));

    /* THE BOUNDARY that defines the whole problem: the two encodings agree
     * through 63 and diverge from 64. */
    for (uint64_t v = 0; v <= 63; v++) {
        uint8_t a[NP_ORACLE_MAX_ENC], b[NP_ORACLE_MAX_ENC];
        size_t na = np_encode(NP_ENC_QUIC_VARINT, v, a);
        size_t nb = np_encode(NP_ENC_VI64, v, b);
        OC(na == 1 && nb == 1 && a[0] == b[0]);
    }
    {
        uint8_t a[NP_ORACLE_MAX_ENC], b[NP_ORACLE_MAX_ENC];
        OC(np_encode(NP_ENC_QUIC_VARINT, 64, a) == 2);
        OC(np_encode(NP_ENC_VI64, 64, b) == 1);
    }

    /*
     * THE QUIET BAND, 64..127, stated EXACTLY -- and this CORRECTS the looser
     * claim carried in this project's earlier notes ("the right value from the
     * wrong number of bytes"), which is true only at 64.
     *
     * QUIC encodes 64..16383 as two bytes: 0x40|(v>>8), v&0xFF. A vi64 reader
     * sees a first byte with no leading 1 bit, so it consumes exactly ONE byte
     * and reports its low 7 bits. Across 64..127 that first byte is always
     * 0x40, so the reader always reports 64:
     *
     *   - it never errors: the read succeeds and the value is in range;
     *   - it consumes 1 of 2 bytes, so the stream desynchronizes by one byte;
     *   - the VALUE survives only at v == 64, and is silently wrong (reported
     *     as 64) for 65..127.
     *
     * Either way a value-only oracle sees nothing, which is the point.
     */
    for (uint64_t v = 64; v <= 127; v++) {
        uint8_t q[NP_ORACLE_MAX_ENC];
        size_t nq = np_encode(NP_ENC_QUIC_VARINT, v, q);
        uint64_t got = ~v;
        size_t used = np_decode(NP_ENC_VI64, q, nq, &got);
        OC(nq == 2 && used == 1 && got == 64);
        OC((v == 64) == (got == v));   /* value-preserving ONLY at 64 */
    }
    /* Just past the band the first byte carries a nonzero high part, so the
     * reported value moves with v while still consuming 1 of 2 bytes. */
    {
        uint8_t q[NP_ORACLE_MAX_ENC];
        uint64_t got = 0;
        size_t nq = np_encode(NP_ENC_QUIC_VARINT, 300, q);
        size_t used = np_decode(NP_ENC_VI64, q, nq, &got);
        OC(nq == 2 && used == 1 && got == 65 && got != 300);
    }
    /* A 4-byte QUIC value is loud in both dimensions: 33333 encodes as
     * 80 00 82 35, from which a vi64 reader takes 2 bytes and reports 0. */
    {
        uint8_t q[NP_ORACLE_MAX_ENC];
        size_t nq = np_encode(NP_ENC_QUIC_VARINT, 33333, q);
        uint64_t got = 1;
        size_t used = np_decode(NP_ENC_VI64, q, nq, &got);
        OC(nq == 4 && used == 2 && got == 0 && got != 33333);
    }

    /* Round trips across the carried boundary inventory. */
    static const uint64_t kBoundaries[] = {
        0, 1, 63, 64, 65, 127, 128, 16383, 16384, 33333,
        1073741823, UINT64_C(1073741824), NP_QUIC_VARINT_MAX,
    };
    for (size_t i = 0; i < sizeof(kBoundaries) / sizeof(kBoundaries[0]); i++) {
        OC(roundtrips(NP_ENC_QUIC_VARINT, kBoundaries[i]));
        OC(roundtrips(NP_ENC_VI64, kBoundaries[i]));
    }
    /* vi64-only reach: every length boundary up to its 9-byte maximum. */
    static const uint64_t kVi64Only[] = {
        2097151, 2097152, 268435455, 268435456,
        UINT64_C(34359738367), UINT64_C(34359738368),
        UINT64_C(4398046511103), UINT64_C(4398046511104),
        UINT64_C(562949953421311), UINT64_C(562949953421312),
        UINT64_C(72057594037927935), UINT64_C(72057594037927936),
        NP_VI64_MAX,
    };
    for (size_t i = 0; i < sizeof(kVi64Only) / sizeof(kVi64Only[0]); i++)
        OC(roundtrips(NP_ENC_VI64, kVi64Only[i]));

    /* REPRESENTABILITY: the QUIC form stops at 2^62-1 and says so. */
    {
        uint8_t buf[NP_ORACLE_MAX_ENC];
        OC(np_encode(NP_ENC_QUIC_VARINT, NP_QUIC_VARINT_MAX, buf) == 8);
        OC(np_encode(NP_ENC_QUIC_VARINT, NP_QUIC_VARINT_MAX + 1, buf) == 0);
        OC(np_encode(NP_ENC_QUIC_VARINT, NP_VI64_MAX, buf) == 0);
        OC(np_encode(NP_ENC_VI64, NP_VI64_MAX, buf) == 9);
    }

    /* MALFORMED / BOUNDS: a short buffer is refused, never over-read. */
    {
        uint8_t q2[2], v9[9];
        uint64_t got = 0;
        (void)np_encode(NP_ENC_QUIC_VARINT, 16383, q2);
        OC(np_decode(NP_ENC_QUIC_VARINT, q2, 1, &got) == 0);
        OC(np_decode(NP_ENC_QUIC_VARINT, q2, 0, &got) == 0);
        (void)np_encode(NP_ENC_VI64, NP_VI64_MAX, v9);
        for (size_t l = 0; l < 9; l++)
            OC(np_decode(NP_ENC_VI64, v9, l, &got) == 0);
        OC(np_decode(NP_ENC_VI64, v9, 9, &got) == 9);
    }

    /* ---- KEY-VALUE-PAIR FRAMING, keyed on the ABSOLUTE Type -------- *
     * The parity that decides the form is the Type's, not the delta's, and
     * the discriminators below are the cases where those two differ. */
    {
        uint8_t out[64];
        static const uint8_t payload[3] = { 0xaa, 0xbb, 0xcc };

        /* even Type, first property (previous 0): delta 2, value 33333 */
        size_t n = np_encode_prop_int(NP_ENC_VI64, 0, 2, 33333,
                                      out, sizeof(out));
        OC(n == 4 && out[0] == 0x02);
        OC(np_encode_prop_int(NP_ENC_QUIC_VARINT, 0, 2, 33333,
                              out, sizeof(out)) == 5);

        /* odd Type: Delta Type, Length, then the bytes */
        n = np_encode_prop_bytes(NP_ENC_VI64, 0, 3, payload, 3,
                                 out, sizeof(out));
        OC(n == 5 && out[0] == 0x03 && out[1] == 0x03 && out[4] == 0xcc);
        /* and the header-only form is exactly that record minus the value */
        OC(np_encode_prop_bytes_header(NP_ENC_VI64, 0, 3, 3,
                                       out, sizeof(out)) == 2);

        /* WRONG FORM for the parity is refused in both directions. */
        OC(np_encode_prop_int(NP_ENC_VI64, 0, 3, 1, out, sizeof(out)) == 0);
        OC(np_encode_prop_bytes(NP_ENC_VI64, 0, 2, payload, 3,
                                out, sizeof(out)) == 0);
        OC(np_encode_prop_bytes_header(NP_ENC_VI64, 0, 2, 3,
                                       out, sizeof(out)) == 0);

        /*
         * THE DISCRIMINATOR: a property following an ODD previous Type, where
         * the delta's parity and the Type's parity DISAGREE. previous 3 -> type
         * 4 is delta 1 (odd) but Type 4 (even), so the integer form is correct
         * and the byte form must be refused. An API keyed on the delta would
         * get both of these backwards.
         */
        n = np_encode_prop_int(NP_ENC_VI64, 3, 4, 9, out, sizeof(out));
        OC(n == 2 && out[0] == 0x01 && out[1] == 0x09);   /* delta 1, value 9 */
        OC(np_encode_prop_bytes(NP_ENC_VI64, 3, 4, payload, 3,
                                out, sizeof(out)) == 0);
        /* and the mirror: previous 4 -> type 7 is delta 3 (odd) AND Type odd */
        n = np_encode_prop_bytes(NP_ENC_VI64, 4, 7, payload, 3,
                                 out, sizeof(out));
        OC(n == 5 && out[0] == 0x03 && out[1] == 0x03);
        /* previous 3 -> type 5: delta 2 (EVEN) but Type 5 (ODD) -> byte form */
        n = np_encode_prop_bytes(NP_ENC_VI64, 3, 5, payload, 3,
                                 out, sizeof(out));
        OC(n == 5 && out[0] == 0x02 && out[1] == 0x03);
        OC(np_encode_prop_int(NP_ENC_VI64, 3, 5, 1, out, sizeof(out)) == 0);

        /* delta 0 is legal: the same Type twice in a row. */
        n = np_encode_prop_int(NP_ENC_VI64, 4, 4, 1, out, sizeof(out));
        OC(n == 2 && out[0] == 0x00);

        /* TYPE ORDER: a decreasing Type has no unsigned delta and is refused
         * (this is the underflow case, and it is refused rather than wrapped). */
        OC(np_encode_prop_int(NP_ENC_VI64, 6, 4, 1, out, sizeof(out)) == 0);
        OC(np_encode_prop_bytes(NP_ENC_VI64, 6, 5, payload, 3,
                                out, sizeof(out)) == 0);
        OC(np_encode_prop_int(NP_ENC_VI64, NP_VI64_MAX, 0, 1,
                              out, sizeof(out)) == 0);
        /* the largest legal delta still encodes: 0 -> 2^64-2 (even Type) */
        OC(np_encode_prop_int(NP_ENC_VI64, 0, NP_VI64_MAX - 1, 1,
                              out, sizeof(out)) == 10);
        /* ... and under draft-16 the same delta is unrepresentable */
        OC(np_encode_prop_int(NP_ENC_QUIC_VARINT, 0, NP_VI64_MAX - 1, 1,
                              out, sizeof(out)) == 0);

        /* THE DRAFT'S LENGTH CEILING: 2^16-1 is legal, one more is not. */
        OC(np_encode_prop_bytes_header(NP_ENC_VI64, 0, 3,
                                       NP_KVP_MAX_VALUE_LEN,
                                       out, sizeof(out)) > 0);
        OC(np_encode_prop_bytes_header(NP_ENC_VI64, 0, 3,
                                       NP_KVP_MAX_VALUE_LEN + 1,
                                       out, sizeof(out)) == 0);

        /*
         * CONSERVATION: every refusal leaves the destination byte-for-byte
         * unchanged. The destination is refilled with a sentinel before each
         * call and the WHOLE buffer is compared afterwards -- an earlier probe
         * compared only the tail, which neither failing call could reach, and
         * therefore proved nothing.
         */
        {
            uint8_t dst[32], ref[32];
            static const uint8_t big[4] = { 1, 2, 3, 4 };
#define NP_CONSERVED(label, call) do {                                        \
            memset(dst, 0xEE, sizeof(dst));                                   \
            memset(ref, 0xEE, sizeof(ref));                                   \
            OC((call) == 0);                                                  \
            OC(memcmp(dst, ref, sizeof(dst)) == 0);                           \
            (void)(label);                                                    \
        } while (0)

            /* the delta fits but the INTEGER does not */
            NP_CONSERVED("int_value_overruns",
                np_encode_prop_int(NP_ENC_VI64, 0, 2, 33333, dst, 2));
            /* the delta fits but the LENGTH does not */
            NP_CONSERVED("length_overruns",
                np_encode_prop_bytes_header(NP_ENC_VI64, 0, 3, 16384, dst, 1));
            /* the HEADER fits but the byte PAYLOAD does not */
            NP_CONSERVED("payload_overruns",
                np_encode_prop_bytes(NP_ENC_VI64, 0, 3, big, 4, dst, 4));
            /* a non-zero length with a NULL payload */
            NP_CONSERVED("null_payload",
                np_encode_prop_bytes(NP_ENC_VI64, 0, 3, NULL, 4, dst,
                                     sizeof(dst)));
            /* a DECREASING Type */
            NP_CONSERVED("decreasing_type",
                np_encode_prop_int(NP_ENC_VI64, 6, 4, 1, dst, sizeof(dst)));
            NP_CONSERVED("decreasing_type_bytes",
                np_encode_prop_bytes(NP_ENC_VI64, 6, 5, big, 4, dst,
                                     sizeof(dst)));
            /* the WRONG value form for the parity */
            NP_CONSERVED("wrong_form_int",
                np_encode_prop_int(NP_ENC_VI64, 0, 3, 1, dst, sizeof(dst)));
            NP_CONSERVED("wrong_form_bytes",
                np_encode_prop_bytes(NP_ENC_VI64, 0, 2, big, 4, dst,
                                     sizeof(dst)));
            /* a Length past the draft ceiling */
            NP_CONSERVED("length_ceiling",
                np_encode_prop_bytes_header(NP_ENC_VI64, 0, 3,
                                            NP_KVP_MAX_VALUE_LEN + 1, dst,
                                            sizeof(dst)));
            /* an UNKNOWN CODEC */
            NP_CONSERVED("unknown_codec",
                np_encode_prop_int((np_enc_t)99, 0, 2, 1, dst, sizeof(dst)));
            /* an unrepresentable delta under draft-16 */
            NP_CONSERVED("unrepresentable_delta",
                np_encode_prop_int(NP_ENC_QUIC_VARINT, 0, NP_VI64_MAX - 1, 1,
                                   dst, sizeof(dst)));

            /* a zero-length payload with a NULL pointer is LEGAL: there is
             * nothing to read, and the header alone is written */
            memset(dst, 0xEE, sizeof(dst));
            OC(np_encode_prop_bytes(NP_ENC_VI64, 0, 3, NULL, 0, dst,
                                    sizeof(dst)) == 2);
            OC(dst[0] == 0x03 && dst[1] == 0x00 && dst[2] == 0xEE);
#undef NP_CONSERVED
        }
    }

    /* AN UNKNOWN CODEC IS REFUSED, never silently treated as vi64. */
    {
        uint8_t out[NP_ORACLE_MAX_ENC];
        uint64_t got = 123;
        np_enc_t bogus = (np_enc_t)99;
        OC(np_encode(bogus, 1, out) == 0);
        OC(np_decode(bogus, (const uint8_t *)"\x01", 1, &got) == 0);
        OC(got == 123);                        /* and *out is untouched */
        OC(np_encode_prop_int(bogus, 0, 2, 1, out, sizeof(out)) == 0);
    }

    return oc_fail;
}
