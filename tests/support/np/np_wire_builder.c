#include "np_wire_builder.h"

#include <stdio.h>
#include <string.h>

/* -- RFC 9626 §3.1 Video Frame Marking --------------------------------- *
 * The flag byte is S|E|I|D|B|TID(3), most significant bit first. A present
 * Layer ID shifts that byte up and occupies the low byte, so the packed value
 * crosses 256 exactly when a Layer ID is present. */

#define NP_VFM_START_OF_FRAME  0x80u
#define NP_VFM_END_OF_FRAME    0x40u
#define NP_VFM_INDEPENDENT     0x20u
#define NP_VFM_DISCARDABLE     0x10u
#define NP_VFM_BASE_LAYER_SYNC 0x08u
#define NP_VFM_TEMPORAL_MASK   0x07u

uint64_t np_pack_video_frame_marking(bool start_of_frame, bool end_of_frame,
                                     bool independent, bool discardable,
                                     bool base_layer_sync, uint8_t temporal_id,
                                     bool has_layer_id, uint8_t layer_id)
{
    if (temporal_id > 7) return UINT64_MAX;

    uint8_t flags = (uint8_t)(temporal_id & NP_VFM_TEMPORAL_MASK);
    if (start_of_frame)  flags |= NP_VFM_START_OF_FRAME;
    if (end_of_frame)    flags |= NP_VFM_END_OF_FRAME;
    if (independent)     flags |= NP_VFM_INDEPENDENT;
    if (discardable)     flags |= NP_VFM_DISCARDABLE;
    if (base_layer_sync) flags |= NP_VFM_BASE_LAYER_SYNC;

    if (has_layer_id)
        return ((uint64_t)flags << 8) | (uint64_t)layer_id;
    return (uint64_t)flags;
}

/* -- The LOC-01 property block ---------------------------------------- */

size_t np_build_loc01_block(np_enc_t enc, const np_loc01_block_t *block,
                            uint8_t *out, size_t cap)
{
    if (!block || !out) return 0;

    /* Ascending identifier order: the delta coding of §1.4.3 requires a
     * nondecreasing Type sequence, and both identifiers here are even, so
     * each value is a bare encoded integer with no Length field. */
    uint64_t types[2];
    uint64_t values[2];
    size_t   n = 0;

    if (block->has_capture_timestamp) {
        types[n]  = NP_LOC01_CAPTURE_TIMESTAMP;
        values[n] = block->capture_timestamp;
        n++;
    }
    if (block->has_video_frame_marking) {
        if (block->video_frame_marking == UINT64_MAX) return 0;  /* refused pack */
        types[n]  = NP_LOC01_VIDEO_FRAME_MARKING;
        values[n] = block->video_frame_marking;
        n++;
    }
    if (n == 0) return 0;

    /* Build whole into a local, then copy once: a destination too small is a
     * refusal that leaves the caller's buffer untouched, never a truncation. */
    uint8_t tmp[NP_LOC01_BLOCK_MAX];
    size_t  pos = 0;
    uint64_t previous_type = 0;
    for (size_t i = 0; i < n; i++) {
        size_t w = np_encode_prop_int(enc, previous_type, types[i], values[i],
                                      tmp + pos, sizeof(tmp) - pos);
        if (w == 0) return 0;
        pos += w;
        previous_type = types[i];
    }

    if (pos > cap) return 0;
    memcpy(out, tmp, pos);
    return pos;
}

/* -- Transport header readers ----------------------------------------- */

/* Type bits shared by draft-16 §10.4.2 and draft-18 §11.4.2. */
#define NP_SG_BIT_PROPERTIES   UINT64_C(0x01)
#define NP_SG_MASK_ID_MODE     UINT64_C(0x06)
#define NP_SG_BIT_END_OF_GROUP UINT64_C(0x08)
#define NP_SG_BIT_STREAM       UINT64_C(0x10)   /* bit 4, always set */
#define NP_SG_BIT_DEFAULT_PRIO UINT64_C(0x20)
#define NP_SG_BIT_FIRST_OBJECT UINT64_C(0x40)   /* draft-18 only */

#define NP_SG_ID_MODE_ZERO     0u
#define NP_SG_ID_MODE_FIRST    1u
#define NP_SG_ID_MODE_PRESENT  2u
#define NP_SG_ID_MODE_RESERVED 3u

/*
 * The VALID TYPE FAMILY, which is narrower per draft and must fail closed --
 * checking only "bit 4 is set" would accept a draft-18-only FIRST_OBJECT type
 * under the draft-16 encoding, and would accept values above either draft's
 * declared range.
 *
 *   draft-16 §10.4.2: the form is 0b00X1XXXX, i.e. 0x10..0x1f and 0x30..0x3f
 *     -- bit 4 set, bits 6 and 7 CLEAR (there is no FIRST_OBJECT bit).
 *   draft-18 §11.4.2: the form is 0b0XX1XXXX, i.e. additionally 0x50..0x5f and
 *     0x70..0x7f -- bit 6 (FIRST_OBJECT) is permitted, bit 7 still clear.
 *
 * Both drafts additionally declare every Subgroup ID Mode 0b11 value invalid.
 */
static bool np_sg_type_in_family(np_enc_t enc, uint64_t type)
{
    if (!(type & NP_SG_BIT_STREAM)) return false;   /* bit 4 always set */
    switch (enc) {
    case NP_ENC_QUIC_VARINT:
        return type <= UINT64_C(0x3f);              /* 0b00X1XXXX */
    case NP_ENC_VI64:
        return type <= UINT64_C(0x7f);              /* 0b0XX1XXXX */
    default:
        return false;                               /* unknown encoding */
    }
}

bool np_read_subgroup_header(np_enc_t enc, const uint8_t *buf, size_t len,
                             np_subgroup_header_t *out)
{
    if (!buf || !out) return false;
    memset(out, 0, sizeof(*out));

    size_t pos = 0;
    size_t w = np_decode(enc, buf + pos, len - pos, &out->type);
    if (w == 0) return false;
    pos += w;

    if (!np_sg_type_in_family(enc, out->type)) return false;

    out->has_properties   = (out->type & NP_SG_BIT_PROPERTIES) != 0;
    out->subgroup_id_mode = (uint8_t)((out->type & NP_SG_MASK_ID_MODE) >> 1);
    out->end_of_group     = (out->type & NP_SG_BIT_END_OF_GROUP) != 0;
    out->default_priority = (out->type & NP_SG_BIT_DEFAULT_PRIO) != 0;
    out->first_object     = (out->type & NP_SG_BIT_FIRST_OBJECT) != 0;
    if (out->subgroup_id_mode == NP_SG_ID_MODE_RESERVED) return false;

    w = np_decode(enc, buf + pos, len - pos, &out->track_alias);
    if (w == 0) return false;
    pos += w;

    w = np_decode(enc, buf + pos, len - pos, &out->group_id);
    if (w == 0) return false;
    pos += w;

    if (out->subgroup_id_mode == NP_SG_ID_MODE_PRESENT) {
        w = np_decode(enc, buf + pos, len - pos, &out->subgroup_id);
        if (w == 0) return false;
        pos += w;
        out->has_subgroup_id = true;
    }

    if (!out->default_priority) {
        if (pos >= len) return false;
        out->publisher_priority = buf[pos];
        pos++;
    }

    return pos == len;   /* exact consumption, or this is not that header */
}

bool np_read_int_header(np_enc_t enc, const uint8_t *buf, size_t len,
                        size_t count, uint64_t *out_values)
{
    if (!buf || !out_values) return false;
    if (count == 0 || count > NP_HEADER_MAX_INTS) return false;

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        size_t w = np_decode(enc, buf + pos, len - pos, &out_values[i]);
        if (w == 0) return false;
        pos += w;
    }
    return pos == len;
}

/* -- Self-check -------------------------------------------------------- */

static int fail(const char *what)
{
    fprintf(stderr, "np_wire_builder: FAIL %s\n", what);
    return 1;
}

static int expect_block(np_enc_t enc, const np_loc01_block_t *b,
                        const uint8_t *want, size_t want_len, const char *what)
{
    uint8_t got[NP_LOC01_BLOCK_MAX];
    size_t n = np_build_loc01_block(enc, b, got, sizeof(got));
    if (n != want_len || memcmp(got, want, want_len) != 0) {
        fprintf(stderr, "np_wire_builder: FAIL %s: got %zu bytes:", what, n);
        for (size_t i = 0; i < n; i++) fprintf(stderr, " %02x", got[i]);
        fprintf(stderr, " want %zu:", want_len);
        for (size_t i = 0; i < want_len; i++) fprintf(stderr, " %02x", want[i]);
        fprintf(stderr, "\n");
        return 1;
    }
    return 0;
}

int np_wire_builder_self_check(void)
{
    int failures = 0;

    /* The packing, against RFC 9626 §3.1 read directly. */
    if (np_pack_video_frame_marking(false, false, true, false, false, 0,
                                    false, 0) != 0x20)
        failures += fail("frame marking: independent alone is 0x20");
    if (np_pack_video_frame_marking(false, false, false, false, false, 0,
                                    false, 0) != 0)
        failures += fail("frame marking: nothing set is 0");
    if (np_pack_video_frame_marking(true, true, true, true, true, 7,
                                    false, 0) != 0xff)
        failures += fail("frame marking: every flag plus TID 7 is 0xff");
    if (np_pack_video_frame_marking(false, false, true, false, false, 0,
                                    true, 0x05) != 0x2005)
        failures += fail("frame marking: a layer id shifts the flag byte up");
    if (np_pack_video_frame_marking(false, false, false, false, false, 8,
                                    false, 0) != UINT64_MAX)
        failures += fail("frame marking: TID above 7 is refused");

    /* The two closure objects, in both encodings. 33 lies inside the band
     * where the two encodings agree byte for byte; 33333 does not, and its
     * two forms are the whole point of the direction pair. */
    np_loc01_block_t small = { true, 33, true, 0x20 };
    static const uint8_t small_both[] = { 0x02, 0x21, 0x02, 0x20 };
    failures += expect_block(NP_ENC_QUIC_VARINT, &small,
                             small_both, sizeof(small_both),
                             "33/indep under the draft-16 (i) encoding");
    failures += expect_block(NP_ENC_VI64, &small,
                             small_both, sizeof(small_both),
                             "33/indep under the draft-18 (vi64) encoding");

    np_loc01_block_t big = { true, 33333, true, 0x00 };
    static const uint8_t big_quic[] =
        { 0x02, 0x80, 0x00, 0x82, 0x35, 0x02, 0x00 };
    static const uint8_t big_vi64[] =
        { 0x02, 0xc0, 0x82, 0x35, 0x02, 0x00 };
    failures += expect_block(NP_ENC_QUIC_VARINT, &big,
                             big_quic, sizeof(big_quic),
                             "33333 under the draft-16 (i) encoding");
    failures += expect_block(NP_ENC_VI64, &big,
                             big_vi64, sizeof(big_vi64),
                             "33333 under the draft-18 (vi64) encoding");

    /* Timestamp only, no marking: the block is one property. */
    np_loc01_block_t ts_only = { true, 33, false, 0 };
    static const uint8_t ts_only_bytes[] = { 0x02, 0x21 };
    failures += expect_block(NP_ENC_VI64, &ts_only,
                             ts_only_bytes, sizeof(ts_only_bytes),
                             "timestamp only is a single property");

    /* Refusals. */
    uint8_t buf[NP_LOC01_BLOCK_MAX];
    np_loc01_block_t none = { false, 0, false, 0 };
    if (np_build_loc01_block(NP_ENC_VI64, &none, buf, sizeof(buf)) != 0)
        failures += fail("an empty block is refused");
    if (np_build_loc01_block((np_enc_t)0, &small, buf, sizeof(buf)) != 0)
        failures += fail("an unknown encoding is refused");
    if (np_build_loc01_block(NP_ENC_VI64, &small, buf, 3) != 0)
        failures += fail("a destination one byte short is refused whole");
    {
        /* Refused whole means UNTOUCHED, and the check must cover the WHOLE
         * backing destination -- including the byte immediately beyond the
         * declared cap. Comparing only bytes 0..cap-1 would let a builder that
         * writes the complete 4-byte block past a 3-byte cap and then reports
         * the refusal pass, which is the worse failure of the two. */
        enum { CAP = 3, BACKING = 8 };
        uint8_t probe[BACKING];
        uint8_t sentinel[BACKING];
        memset(sentinel, 0xee, sizeof(sentinel));
        memcpy(probe, sentinel, sizeof(probe));
        size_t n = np_build_loc01_block(NP_ENC_VI64, &small, probe, CAP);
        if (n != 0)
            failures += fail("a destination one byte short reports a refusal");
        if (memcmp(probe, sentinel, sizeof(probe)) != 0) {
            fprintf(stderr,
                "np_wire_builder: FAIL a refused build wrote into the "
                "destination (cap %d, backing %d):", (int)CAP, (int)BACKING);
            for (size_t i = 0; i < sizeof(probe); i++)
                fprintf(stderr, " %02x%s", probe[i],
                        i + 1 == (size_t)CAP ? " |" : "");
            fprintf(stderr, "\n");
            failures++;
        }
    }
    {
        np_loc01_block_t over = { true, NP_VI64_MAX, false, 0 };
        if (np_build_loc01_block(NP_ENC_QUIC_VARINT, &over,
                                 buf, sizeof(buf)) != 0)
            failures += fail("a value beyond the (i) range is refused");
        if (np_build_loc01_block(NP_ENC_VI64, &over, buf, sizeof(buf)) == 0)
            failures += fail("the same value is representable as (vi64)");
    }

    /* The subgroup-header reader. 0x1d = stream bit | properties |
     * Subgroup ID Mode 0b10 | End of Group, which is what a properties-bearing
     * end-of-group subgroup carries in BOTH drafts. */
    {
        static const uint8_t hdr[] = { 0x1d, 0x02, 0x00, 0x00, 0x80 };
        np_subgroup_header_t h;
        for (int i = 0; i < 2; i++) {
            np_enc_t enc = i == 0 ? NP_ENC_QUIC_VARINT : NP_ENC_VI64;
            if (!np_read_subgroup_header(enc, hdr, sizeof(hdr), &h))
                failures += fail("subgroup header 0x1d is readable");
            else if (h.type != 0x1d || !h.has_properties ||
                     h.subgroup_id_mode != NP_SG_ID_MODE_PRESENT ||
                     !h.end_of_group || h.default_priority ||
                     h.track_alias != 2 || h.group_id != 0 ||
                     !h.has_subgroup_id || h.subgroup_id != 0 ||
                     h.publisher_priority != 0x80)
                failures += fail("subgroup header 0x1d decodes field for field");
        }
        /* One trailing byte is not that header. */
        static const uint8_t trailing[] = { 0x1d, 0x02, 0x00, 0x00, 0x80, 0x00 };
        if (np_read_subgroup_header(NP_ENC_VI64, trailing, sizeof(trailing), &h))
            failures += fail("a trailing byte makes it not a subgroup header");
        /* One byte short. */
        if (np_read_subgroup_header(NP_ENC_VI64, hdr, sizeof(hdr) - 1, &h))
            failures += fail("a truncated subgroup header is refused");
        /* Reserved Subgroup ID Mode 0b11. */
        static const uint8_t reserved[] = { 0x1f, 0x02, 0x00, 0x00, 0x80 };
        if (np_read_subgroup_header(NP_ENC_VI64, reserved, sizeof(reserved), &h))
            failures += fail("reserved Subgroup ID Mode 0b11 is refused");
        /* Bit 4 clear is a datagram type, not a stream header. */
        static const uint8_t datagram[] = { 0x0d, 0x02, 0x00, 0x00, 0x80 };
        if (np_read_subgroup_header(NP_ENC_VI64, datagram, sizeof(datagram), &h))
            failures += fail("a type with bit 4 clear is not a subgroup header");
        /* An unknown encoding decodes nothing. */
        if (np_read_subgroup_header((np_enc_t)0, hdr, sizeof(hdr), &h))
            failures += fail("an unknown encoding reads no subgroup header");
        if (np_read_subgroup_header(NP_ENC_VI64, NULL, 5, &h) ||
            np_read_subgroup_header(NP_ENC_VI64, hdr, sizeof(hdr), NULL))
            failures += fail("NULL arguments are refused");
    }

    /* The VALID TYPE FAMILY is per draft, and the reader must fail closed on the
     * difference rather than accepting anything with bit 4 set. */
    {
        np_subgroup_header_t h;

        /* EVERY out-of-family vector below is built so that the header
         * OTHERWISE PARSES EXACTLY -- correct field set for its own mode and
         * priority bits, buffer consumed whole. Only the family check can
         * reject it, so a bit-4-only reader admits it and the assertion is
         * genuinely load-bearing. (Vectors that merely fail exact consumption
         * would pass with the check removed, which is how the first attempt at
         * this block failed to discriminate.)
         *
         * 0x5d = FIRST_OBJECT | bit4 | EOG | mode PRESENT | properties: a legal
         * draft-18 form. Draft-16 has no FIRST_OBJECT bit and no 0x50 family,
         * and 0x5d is above 63 so its (i) form is the two-byte 40 5d. */
        static const uint8_t fo_vi64[] = { 0x5d, 0x02, 0x00, 0x00, 0x80 };
        static const uint8_t fo_quic[] = { 0x40, 0x5d, 0x02, 0x00, 0x00, 0x80 };
        if (!np_read_subgroup_header(NP_ENC_VI64, fo_vi64,
                                     sizeof(fo_vi64), &h))
            failures += fail("draft-18 accepts a FIRST_OBJECT subgroup type");
        else if (h.type != 0x5d || !h.first_object || !h.has_properties ||
                 h.subgroup_id_mode != NP_SG_ID_MODE_PRESENT ||
                 !h.end_of_group || h.default_priority ||
                 h.track_alias != 2 || h.publisher_priority != 0x80)
            failures += fail("the FIRST_OBJECT type decodes field for field");
        if (np_read_subgroup_header(NP_ENC_QUIC_VARINT, fo_quic,
                                    sizeof(fo_quic), &h))
            failures += fail("draft-16 rejects the FIRST_OBJECT type family");

        /* Type 144 (0x90): bit 4 IS set, so a bit-4-only check admits it, but
         * it is above BOTH drafts' declared ranges. Mode 0b00 (no Subgroup ID)
         * and bit 5 clear (Priority present) make both forms consume exactly. */
        static const uint8_t high_vi64[] = { 0x80, 0x90, 0x02, 0x00, 0x80 };
        static const uint8_t high_quic[] = { 0x40, 0x90, 0x02, 0x00, 0x80 };
        if (np_read_subgroup_header(NP_ENC_VI64, high_vi64,
                                    sizeof(high_vi64), &h))
            failures += fail("draft-18 rejects a type above its range");
        if (np_read_subgroup_header(NP_ENC_QUIC_VARINT, high_quic,
                                    sizeof(high_quic), &h))
            failures += fail("draft-16 rejects a type above its range");

        /* The family check must not over-reject: the DEFAULT_PRIORITY families
         * are legal in their own drafts and carry no priority byte. */
        static const uint8_t d16_defprio[] = { 0x3d, 0x02, 0x00, 0x00 };
        if (!np_read_subgroup_header(NP_ENC_QUIC_VARINT, d16_defprio,
                                     sizeof(d16_defprio), &h))
            failures += fail("draft-16 accepts its 0x30 default-priority family");
        else if (!h.default_priority || h.first_object)
            failures += fail("0x3d is default-priority and not first-object");
        static const uint8_t d18_defprio[] = { 0x7d, 0x02, 0x00, 0x00 };
        if (!np_read_subgroup_header(NP_ENC_VI64, d18_defprio,
                                     sizeof(d18_defprio), &h))
            failures += fail("draft-18 accepts its 0x70 family");
        else if (!h.default_priority || !h.first_object)
            failures += fail("0x7d is default-priority AND first-object");
        /* Draft-16 must reject the 0x70 family. 0x7d is above 63, so its (i)
         * form is the two-byte 40 7d; bit 5 is set so no Priority byte follows
         * and mode PRESENT supplies the Subgroup ID -- consumed exactly. */
        static const uint8_t d18_defprio_quic[] = { 0x40, 0x7d, 0x02, 0x00, 0x00 };
        if (np_read_subgroup_header(NP_ENC_QUIC_VARINT, d18_defprio_quic,
                                    sizeof(d18_defprio_quic), &h))
            failures += fail("draft-16 rejects the 0x70 family");

        /* Reserved mode 0b11 stays rejected inside an otherwise valid family. */
        static const uint8_t reserved_hi[] = { 0x5f, 0x02, 0x00, 0x00, 0x80 };
        if (np_read_subgroup_header(NP_ENC_VI64, reserved_hi,
                                    sizeof(reserved_hi), &h))
            failures += fail("reserved mode is rejected in the 0x50 family too");
    }

    /* The integer-header reader, and the disjointness the classifier needs. */
    {
        uint64_t v[NP_HEADER_MAX_INTS];
        static const uint8_t two[] = { 0x00, 0x04 };   /* delta 0, props len 4 */
        static const uint8_t one[] = { 0x01 };         /* payload len 1 */
        if (!np_read_int_header(NP_ENC_VI64, two, sizeof(two), 2, v) ||
            v[0] != 0 || v[1] != 4)
            failures += fail("a two-integer header reads as two integers");
        if (np_read_int_header(NP_ENC_VI64, two, sizeof(two), 1, v))
            failures += fail("a two-byte header is not one whole integer");
        if (!np_read_int_header(NP_ENC_VI64, one, sizeof(one), 1, v) || v[0] != 1)
            failures += fail("a one-integer header reads as one integer");
        if (np_read_int_header(NP_ENC_VI64, one, sizeof(one), 2, v))
            failures += fail("a one-byte header is not two integers");
        if (np_read_int_header(NP_ENC_VI64, two, sizeof(two),
                               NP_HEADER_MAX_INTS + 1, v))
            failures += fail("too many integers requested is refused");
    }

    return failures;
}
