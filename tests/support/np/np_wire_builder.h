/*
 * Negotiated-profile substrate, ROLE 2 of 4: the SCRIPTED WIRE BUILDER.
 *
 * This role writes and reads the bytes the drafts specify, using the pure
 * oracle (role 1) and the C standard library ONLY. It deliberately calls no
 * product LOC helper, no product key-value-pair codec and no product
 * integer codec: a builder that reused any of them would inherit the very
 * bug the closure directions exist to detect, and both directions would then
 * agree while both were wrong. That independence is enforced outside this
 * file by tests/cmake/check_np_roles.cmake, which scans this SOURCE for the
 * forbidden identifiers and this compiled OBJECT for the corresponding
 * symbols, defined or undefined.
 *
 * Two jobs, one per closure direction:
 *
 *   BUILD  the LOC-01 object-property block for a chosen draft, so a real
 *          service receiver can be fed bytes no product encoder produced;
 *   READ   the transport-level subgroup and object headers, so a captured
 *          send action can be classified by DECODED IDENTITY rather than by
 *          position, adjacency or arrival order.
 *
 * Sources re-derived here, not copied from the implementation:
 *
 *   draft-ietf-moq-loc-01 §2.3       -- LOC-01 property identifiers, and the
 *                                       Capture Timestamp's microseconds-since-
 *                                       the-Unix-epoch meaning.
 *   RFC 9626 §3.1                    -- the Video Frame Marking bit layout.
 *   draft-ietf-moq-transport-16 §10.4.2 and
 *   draft-ietf-moq-transport-18 §11.4.2 -- SUBGROUP_HEADER. The two drafts
 *       define the SAME type bits for every field read here (0x01 the
 *       properties/extensions bit, 0x06 the two-bit Subgroup ID Mode, 0x08
 *       End of Group, 0x20 Default Publisher Priority); draft-18 adds 0x40
 *       FIRST_OBJECT, which this reader reports but does not need. Only the
 *       INTEGER ENCODING of the fields differs, which is exactly the np_enc_t
 *       argument. One reader is therefore correct for both, and that is a
 *       read of the two drafts rather than an assumption.
 */
#ifndef NP_WIRE_BUILDER_H
#define NP_WIRE_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "np_oracle.h"

/* -- LOC-01 property identifiers (draft-ietf-moq-loc-01 §2.3) ---------- */

#define NP_LOC01_CAPTURE_TIMESTAMP   UINT64_C(0x02)
#define NP_LOC01_VIDEO_FRAME_MARKING UINT64_C(0x04)
#define NP_LOC01_AUDIO_LEVEL         UINT64_C(0x06)
#define NP_LOC01_VIDEO_CONFIG        UINT64_C(0x0d)

/* Enough for the blocks this role builds; every builder refuses rather than
 * truncates, so a caller may also pass a smaller buffer deliberately. */
#define NP_LOC01_BLOCK_MAX 64

/*
 * The two LOC-01 properties a RAW video object carries. Both identifiers are
 * EVEN, so under §1.4.3 each value is a single encoded integer with no Length
 * -- which is precisely why the draft's integer encoding decides the bytes.
 */
typedef struct np_loc01_block {
    bool     has_capture_timestamp;
    uint64_t capture_timestamp;       /* microseconds since the Unix epoch */
    bool     has_video_frame_marking;
    uint64_t video_frame_marking;     /* packed; see np_pack_video_frame_marking */
} np_loc01_block_t;

/*
 * Pack a Video Frame Marking value from its RFC 9626 §3.1 fields. The layer
 * id, when present, occupies the low byte and shifts the flag byte up -- so a
 * value >= 256 carries a layer id, which is how a reader tells the two forms
 * apart.
 *
 * Returns the packed value, or UINT64_MAX if temporal_id exceeds 7 (the field
 * is three bits wide), which no caller may then encode.
 */
uint64_t np_pack_video_frame_marking(bool start_of_frame, bool end_of_frame,
                                     bool independent, bool discardable,
                                     bool base_layer_sync, uint8_t temporal_id,
                                     bool has_layer_id, uint8_t layer_id);

/*
 * Build the complete LOC-01 property block for `enc`, emitting the present
 * properties in ASCENDING identifier order (0x02 then 0x04) as the delta
 * coding requires.
 *
 * Returns the byte count, or 0 on any refusal: no property present, a value
 * the encoding cannot represent, or a destination too small. Never truncates.
 */
size_t np_build_loc01_block(np_enc_t enc, const np_loc01_block_t *block,
                            uint8_t *out, size_t cap);

/* -- Transport header readers, for classifying a captured send action -- */

typedef struct np_subgroup_header {
    uint64_t type;
    uint64_t track_alias;
    uint64_t group_id;
    bool     has_properties;      /* type & 0x01 */
    uint8_t  subgroup_id_mode;    /* (type & 0x06) >> 1 */
    bool     end_of_group;        /* type & 0x08 */
    bool     default_priority;    /* type & 0x20 */
    bool     first_object;        /* type & 0x40 (draft-18 only) */
    bool     has_subgroup_id;     /* mode 0b10 */
    uint64_t subgroup_id;
    uint8_t  publisher_priority;  /* valid when !default_priority */
} np_subgroup_header_t;

/*
 * Read a SUBGROUP_HEADER from buf[0..len). Returns true only when the header
 * is well formed for `enc` AND consumes the buffer EXACTLY -- a trailing byte
 * makes it false. Exact consumption is what lets a caller distinguish a
 * stream-opening header from any other captured header without looking at
 * where the record sat in the stream.
 *
 * Refuses a reserved Subgroup ID Mode of 0b11, and refuses a type whose bit 4
 * is clear (both drafts require it set).
 */
bool np_read_subgroup_header(np_enc_t enc, const uint8_t *buf, size_t len,
                             np_subgroup_header_t *out);

/*
 * Read a header consisting of EXACTLY `count` integers and nothing else.
 * Returns true only on exact consumption. `count` may be at most
 * NP_HEADER_MAX_INTS.
 *
 * The two object headers this substrate must tell apart are exactly this
 * shape: a properties-bearing object's header is [Object ID Delta][Properties
 * Length] (two integers, header only -- the property bytes travel as the
 * action's own payload), while the following payload record's header is
 * [Payload Length] (one integer). With both lengths below 64 the two encodings
 * are one byte per integer in either draft, so "exactly two integers,
 * consumed whole" and "exactly one integer, consumed whole" are disjoint --
 * no ordering assumption is needed to separate them.
 */
#define NP_HEADER_MAX_INTS 4

bool np_read_int_header(np_enc_t enc, const uint8_t *buf, size_t len,
                        size_t count, uint64_t *out_values);

/*
 * Run this role's own known-answer and refusal self-checks (LOC-01 blocks for
 * both encodings, the frame-marking packing, and the header readers including
 * their exact-consumption refusals). Returns the number of failures; 0 means
 * the role may be trusted. Quiet on success.
 */
int np_wire_builder_self_check(void);

#endif /* NP_WIRE_BUILDER_H */
