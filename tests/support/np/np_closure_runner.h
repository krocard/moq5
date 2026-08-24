/*
 * Negotiated-profile substrate, ROLE 4 of 4: the CLOSURE RUNNER.
 *
 * This role stands the two closure directions up and reports what happened.
 * It never judges: every expectation lives in the test that calls it, built
 * from the oracle and the scripted builder, so the assertions are independent
 * of the bytes and of the product behaviour they judge.
 *
 * It may drive sessions, the SimPair harness and the media service facades.
 * It may NOT call the product's LOC helpers or its key-value-pair codec --
 * enforced outside this file by tests/cmake/check_np_roles.cmake, on this
 * SOURCE and on this compiled OBJECT. All wire reading it needs comes from
 * role 2 (np_wire_builder), which in turn uses role 1 (np_oracle) only.
 *
 * DIRECTION A -- a real media_sender, driven through the endpoint-free drive
 * seam against a real SimPair peer, emits LOC-01 object properties. The bytes
 * are captured from the SimPair trace and handed back verbatim.
 *
 * DIRECTION B -- independently built property bytes are published by the real
 * peer session to a real media_receiver, whose public poll output is handed
 * back verbatim.
 *
 * The negotiated draft is READ from the session (moq_session_version) in both
 * directions and reported; nothing is injected into either facade.
 */
#ifndef NP_CLOSURE_RUNNER_H
#define NP_CLOSURE_RUNNER_H

#include <moq/media_receiver.h>
#include <moq/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "np_wire_builder.h"

#define NP_CLOSURE_MAX_OBJECTS   4
#define NP_CLOSURE_PROP_MAX      64
#define NP_CLOSURE_PAYLOAD_MAX   32
#define NP_CLOSURE_MAX_STREAMS   8

/* ===================================================================== *
 *  Direction A -- real service sender to independently checked bytes
 * ===================================================================== */

typedef struct np_sender_object {
    uint64_t    capture_time_us;
    bool        is_sync;
    bool        starts_group;
    const char *payload;      /* NUL-terminated; length < 64 keeps the payload
                               * record's own header a single integer, which is
                               * what makes the classification disjoint */
} np_sender_object_t;

/* One captured object-properties send: the header said "this many property
 * bytes follow", and these are those bytes. */
typedef struct np_captured_prop {
    uint64_t object_id;      /* derived from the delta chain on this stream */
    uint64_t declared_len;   /* the header's Properties Length */
    size_t   len;            /* bytes actually carried */
    uint8_t  bytes[NP_CLOSURE_PROP_MAX];
} np_captured_prop_t;

typedef struct np_captured_payload {
    uint64_t declared_len;   /* the header's Payload Length */
    size_t   len;
    uint8_t  bytes[NP_CLOSURE_PAYLOAD_MAX];
} np_captured_payload_t;

typedef struct np_sender_result {
    /* Setup: true only when every step reached its stated goal. When false,
     * setup_failure names the step that did not, and nothing below is
     * meaningful. */
    bool        setup_ok;
    const char *setup_failure;

    moq_version_t version_before_first_write;  /* read before object 1 */
    moq_session_state_t state_before_first_write;

    uint64_t track_alias;         /* from the peer's SUBSCRIBE_OK */
    bool     have_track_alias;

    /* Every distinct data stream whose FIRST captured send read as a whole
     * subgroup header, and the one carrying the video track alias. */
    size_t   subgroup_stream_count;
    bool     have_subgroup;
    np_subgroup_header_t subgroup;

    size_t   prop_count;
    np_captured_prop_t props[NP_CLOSURE_MAX_OBJECTS];
    size_t   payload_count;
    np_captured_payload_t payloads[NP_CLOSURE_MAX_OBJECTS];
    /* Sends on the media subgroup that read as neither shape. A nonzero count
     * means the classification is incomplete and must not be trusted. */
    size_t   unclassified_count;
    /* Sends on ANY other stream, so a record on an undeclared or undecodable
     * stream is accounted for instead of silently vanishing from the totals. */
    size_t   other_stream_records;
    /* Records whose trace legacy payload count disagreed with the bytes
     * actually carried (including a capture the detail buffer could not hold).
     * Asserted zero, so the legacy count is genuinely consumed rather than
     * ignored. */
    size_t   count_mismatch_records;

    /* write() results, in call order. */
    size_t       write_count;
    moq_result_t write_rc[NP_CLOSURE_MAX_OBJECTS];

    /* State after the bounded pumps. */
    bool     sender_fatal;
    uint64_t sender_fatal_code;
    bool     sender_closed;
    uint64_t objects_written;
    uint64_t objects_sent;
    uint64_t objects_dropped;
    uint64_t groups_dropped;
    uint64_t groups_abandoned;
    moq_result_t sender_last_error;
    moq_session_state_t session_state_after;

    /* Trace bookkeeping: an overflow means captures were lost and the
     * classification above is not exhaustive. */
    bool     trace_overflow;
    size_t   send_data_records;
} np_sender_result_t;

/*
 * Run direction A at `version`. Returns MOQ_OK when the run completed (which
 * says nothing about whether the bytes were right -- that is the caller's
 * judgement); MOQ_ERR_INVAL on a bad argument. Never asserts.
 */
moq_result_t np_closure_run_sender(moq_version_t version,
                                   const np_sender_object_t *objects,
                                   size_t object_count,
                                   np_sender_result_t *out);

/* ===================================================================== *
 *  Direction B -- independently built bytes to a real service receiver
 * ===================================================================== */

typedef struct np_recv_input {
    const uint8_t *properties;    /* built by role 2; never by a product encoder */
    size_t         properties_len;
    const char    *payload;       /* NUL-terminated */
} np_recv_input_t;

/*
 * What the public poll surfaced. The receiver's object carries no group or
 * object id (moq_media_object_t has none), so an object is identified by its
 * distinct payload bytes -- which is why each input carries a different
 * payload. `keyframe` is reported because on a RAW/LOC track it is decoded
 * from the LOC Video Frame Marking property, so it is a second, independent
 * observation of the same property block.
 */
typedef struct np_recv_object {
    bool     has_capture_time;
    uint64_t capture_time_us;
    bool     keyframe;
    size_t   len;
    uint8_t  bytes[NP_CLOSURE_PAYLOAD_MAX];
} np_recv_object_t;

typedef struct np_receiver_result {
    bool        setup_ok;
    const char *setup_failure;

    moq_version_t version_before_first_write;
    moq_session_state_t state_before_first_write;

    /* Discovery, counted rather than latched: exactly one of each is correct,
     * and a duplicate or a foreign track event must be visible. */
    size_t   track_added_count;
    size_t   catalog_ready_count;
    size_t   other_track_events;
    bool     track_added;
    bool     catalog_ready;

    /* The peer's inbound subscription requests, classified EXACTLY: the
     * namespace must be svc/demo and the name must be the catalog track or the
     * one media track. Anything else -- a duplicate, a foreign name, a foreign
     * namespace -- lands in the unexpected bucket with its name recorded, and
     * every other session event on the peer is counted too. */
    size_t   sub_catalog_count;
    size_t   sub_video_count;
    size_t   sub_unexpected_count;
    char     sub_unexpected_name[32];
    size_t   other_session_events;
    uint32_t other_session_event_kind;   /* the last one seen, for diagnosis */
    moq_result_t accept_catalog_rc;
    moq_result_t accept_video_rc;

    /* Publisher side: what the scripted peer actually got onto the wire. */
    size_t       published_count;
    moq_result_t publish_rc[NP_CLOSURE_MAX_OBJECTS];

    /* Receiver side: the public poll output, verbatim. */
    size_t          object_count;
    np_recv_object_t objects[NP_CLOSURE_MAX_OBJECTS];

    uint64_t objects_received;
    uint64_t objects_dropped;
    uint64_t parse_drops;
    uint64_t catalog_drops;
    /* The coalesced per-track parse-drop diagnostic, so the accounting is
     * observed as an EVENT with a class and not only as a counter. */
    size_t                       parse_drop_events;
    moq_media_parse_drop_class_t parse_drop_last_class;
    uint64_t                     parse_drops_reported_total;
    bool     receiver_fatal;
    uint64_t receiver_fatal_code;
    moq_session_state_t session_state_after;
} np_receiver_result_t;

/*
 * Run direction B at `version`, publishing each input as one object of a
 * single RAW LOC video track that the receiver discovers through a real MSF
 * catalog. Returns MOQ_OK when the run completed. Never asserts.
 */
moq_result_t np_closure_run_receiver(moq_version_t version,
                                     const np_recv_input_t *inputs,
                                     size_t input_count,
                                     np_receiver_result_t *out);

/*
 * Map a negotiated draft onto the integer encoding that draft specifies.
 * Returns 0 for a version this substrate does not model, which a caller must
 * treat as a refusal rather than a default.
 */
np_enc_t np_enc_for_version(moq_version_t version);

#endif /* NP_CLOSURE_RUNNER_H */
