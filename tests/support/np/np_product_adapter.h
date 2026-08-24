/*
 * Negotiated-profile substrate, ROLE 3 of 4: the PRODUCT ADAPTER.
 *
 * This role is the ONLY translation unit in the substrate permitted to call
 * the product's LOC API. Roles 1, 2 and 4 are forbidden it -- the oracle and
 * the builder because an oracle that called the implementation would agree
 * with it by construction, the runner because a driver that reached past its
 * two subjects could no longer be said to have compared them.
 *
 * Concentrating the product calls here is what makes that enforceable:
 * tests/cmake/check_np_roles.cmake scans the other three roles' SOURCES and
 * OBJECTS for moq_loc_ / moq_kvp_ / integer-codec identifiers, and a call
 * that leaked into one of them would appear as a forbidden token or, if it
 * arrived through a helper, as an undefined symbol. The adapter's own
 * boundary is DECLARED in that file, not scanned; what the policy enforces
 * is that the calls are HERE and nowhere else.
 *
 * Interface shape: the product's LOC headers are TYPED fields, while the
 * scripted builder works in the packed integer the wire carries. This role
 * deliberately mirrors the PRODUCT's typed shape rather than the builder's
 * packed one, so the packing itself stays the builder's business and is
 * never re-derived on this side of the comparison.
 */
#ifndef NP_PRODUCT_ADAPTER_H
#define NP_PRODUCT_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <moq/session.h>   /* moq_version_t */

/* The LOC-01 fields this substrate exercises, in the product's typed form. */
typedef struct np_adapter_loc01 {
    bool     has_capture_timestamp;
    uint64_t capture_timestamp;

    bool     has_video_frame_marking;
    bool     vfm_start_of_frame;
    bool     vfm_end_of_frame;
    bool     vfm_independent;
    bool     vfm_discardable;
    bool     vfm_base_layer_sync;
    uint8_t  vfm_temporal_id;
    bool     vfm_has_layer_id;
    uint8_t  vfm_layer_id;
} np_adapter_loc01_t;

/*
 * Encode `in` as a LOC-01 property block for `transport_version` using the
 * PRODUCT encoder, copying the result into out[0..cap).
 *
 * Returns the byte count, or 0 on any refusal -- an unsupported version, a
 * value the selected codec cannot represent, a field the profile rejects, or
 * a destination too small. Never truncates, so a 0 is always a refusal and
 * never a partial block.
 */
size_t np_adapter_encode_loc01(moq_version_t transport_version,
                               const np_adapter_loc01_t *in,
                               uint8_t *out, size_t cap);

/*
 * Parse buf[0..len) as a LOC-01 property block for `transport_version` using
 * the PRODUCT parser. Returns true on success and fills *out; returns false
 * if the product rejected the bytes.
 */
bool np_adapter_parse_loc01(moq_version_t transport_version,
                            const uint8_t *buf, size_t len,
                            np_adapter_loc01_t *out);

#endif /* NP_PRODUCT_ADAPTER_H */
