#include "np_closure_runner.h"

#include <moq/media_receiver.h>
#include <moq/media_sender.h>
#include <moq/rcbuf.h>
#include <moq/sim.h>
#include <moq/types.h>

#include <string.h>

/* -- The service drive seams (compiled with the testing defines) -------- */

moq_media_sender_t *moq_media_sender_test_new_cfg(
    const moq_media_sender_cfg_t *cfg);
void moq_media_sender_test_pump(moq_media_sender_t *s,
                                moq_session_t *session, uint64_t now_us);
void moq_media_sender_test_free(moq_media_sender_t *s);

moq_media_receiver_t *moq_media_receiver_test_new_cfg(
    const moq_media_receiver_cfg_t *cfg);
void moq_media_receiver_test_pump(moq_media_receiver_t *r,
                                  moq_session_t *session, uint64_t now_us);
/* The SESSION-AWARE teardown seam, not the raw shell free: the pumped
 * receiver_hook creates the subscriber facade, and only this seam runs the
 * production teardown path (live-handle snapshot, per-handle UNSUBSCRIBE on the
 * supplied session, then facade destroy) before freeing the receiver. */
void moq_media_receiver_test_destroy_with_session(moq_media_receiver_t *r,
                                                  moq_session_t *session,
                                                  uint64_t now_us);

/* -- Shared fixture constants ----------------------------------------- */

#define NP_TRACK_NAME     "v"
#define NP_CATALOG_NAME   "catalog"
#define NP_PUMP_TICK_US   1000u
#define NP_QUIESCE_STEPS  16u

static moq_bytes_t np_lit(const char *s)
{
    moq_bytes_t b;
    b.data = (const uint8_t *)s;
    b.len = strlen(s);
    return b;
}

np_enc_t np_enc_for_version(moq_version_t version)
{
    switch (version) {
    case MOQ_VERSION_DRAFT_16: return NP_ENC_QUIC_VARINT;
    case MOQ_VERSION_DRAFT_18: return NP_ENC_VI64;
    default: return (np_enc_t)0;
    }
}

/* ===================================================================== *
 *  Trace capture (direction A)
 * ===================================================================== */

#define NP_TRACE_CAP 64

typedef struct np_trace_send {
    uint64_t stream_ref;
    size_t   header_len;
    uint8_t  header[32];
    size_t   detail_len;
    uint8_t  detail[NP_CLOSURE_PROP_MAX];
    size_t   declared_payload_len;   /* the record's legacy count */
    bool     detail_truncated;
} np_trace_send_t;

typedef struct np_trace_ctx {
    size_t          count;
    bool            overflow;
    np_trace_send_t rec[NP_TRACE_CAP];
} np_trace_ctx_t;

/* Only client-originated SEND_DATA actions are of interest: those are the
 * publisher's data-stream writes. Every field read here is declared for
 * SEND_DATA by the trace contract -- the header in `bytes`, the payload length
 * in `count`, the payload itself in `detail_bytes` -- and both spans are
 * BORROWED for the callback only, so they are deep-copied here. */
static void np_trace_cb(void *ctx, const moq_sim_trace_record_t *r)
{
    np_trace_ctx_t *t = (np_trace_ctx_t *)ctx;
    if (!t || !r) return;
    if (r->kind != MOQ_SIM_TRACE_ACTION) return;
    if (r->action_kind != MOQ_ACTION_SEND_DATA) return;
    if (r->from != MOQ_PERSPECTIVE_CLIENT) return;
    /* The appended fields must be present in this record's declared extent
     * before any of them is read. */
    if (r->struct_size < offsetof(moq_sim_trace_record_t, detail_bytes) +
                         sizeof(r->detail_bytes))
        return;

    if (t->count >= NP_TRACE_CAP) { t->overflow = true; return; }
    np_trace_send_t *d = &t->rec[t->count];
    memset(d, 0, sizeof(*d));
    d->stream_ref = r->stream_ref._v;
    d->declared_payload_len = r->count;
    if (r->bytes.data && r->bytes.len <= sizeof(d->header)) {
        d->header_len = r->bytes.len;
        memcpy(d->header, r->bytes.data, r->bytes.len);
    } else if (r->bytes.len > sizeof(d->header)) {
        d->header_len = 0;   /* longer than any object/subgroup header can be */
    }
    if (r->detail_bytes.data && r->detail_bytes.len > 0) {
        if (r->detail_bytes.len <= sizeof(d->detail)) {
            d->detail_len = r->detail_bytes.len;
            memcpy(d->detail, r->detail_bytes.data, r->detail_bytes.len);
        } else {
            d->detail_truncated = true;
        }
    }
    t->count++;
}

/* ===================================================================== *
 *  Direction A
 * ===================================================================== */

static void np_drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) == 1)
        moq_event_cleanup(&ev);
}

/* Classify the captured sends. Selection is by DECODED IDENTITY: a stream is
 * the media subgroup only when its first captured send reads as a whole
 * subgroup header carrying the alias the peer's SUBSCRIBE_OK named, and a send
 * on that stream is a properties send only when its header reads as exactly
 * two integers whose second equals the bytes carried.
 *
 * ORDER, stated precisely rather than denied: this uses the order the protocol
 * MANDATES WITHIN ONE SUBGROUP STREAM -- the stream opens with its Subgroup
 * Header, each object's Object ID Delta continues that stream's delta chain,
 * and an object's payload follows its own properties. It does NOT use
 * incidental global action position, cross-stream adjacency, or any equality
 * between an ACTION ref and an INPUT ref (SimPair remaps refs across the pair,
 * so those two are never compared). "First send on this stream" is a
 * within-stream protocol fact, not a position in the global capture. */
static void np_classify_sender_trace(const np_trace_ctx_t *t, np_enc_t enc,
                                     np_sender_result_t *out)
{
    uint64_t refs[NP_CLOSURE_MAX_STREAMS];
    size_t   first_idx[NP_CLOSURE_MAX_STREAMS];
    size_t   ref_count = 0;

    /* The trace's legacy payload count is CONSUMED, not ignored: for every
     * SEND_DATA it must equal the bytes the record actually carried. A capture
     * too large for the detail buffer reports here as well, so a truncation
     * cannot masquerade as agreement. */
    for (size_t i = 0; i < t->count; i++) {
        const np_trace_send_t *r = &t->rec[i];
        if (r->detail_truncated ||
            r->declared_payload_len != r->detail_len)
            out->count_mismatch_records++;
    }

    for (size_t i = 0; i < t->count; i++) {
        bool seen = false;
        for (size_t k = 0; k < ref_count; k++)
            if (refs[k] == t->rec[i].stream_ref) { seen = true; break; }
        if (seen) continue;
        if (ref_count >= NP_CLOSURE_MAX_STREAMS) continue;
        refs[ref_count] = t->rec[i].stream_ref;
        first_idx[ref_count] = i;
        ref_count++;
    }

    bool     have = false;
    uint64_t video_ref = 0;
    for (size_t k = 0; k < ref_count; k++) {
        np_subgroup_header_t h;
        const np_trace_send_t *r = &t->rec[first_idx[k]];
        if (!np_read_subgroup_header(enc, r->header, r->header_len, &h))
            continue;
        out->subgroup_stream_count++;
        if (!out->have_track_alias || h.track_alias != out->track_alias)
            continue;
        if (have) continue;   /* a second one is reported by the count above */
        have = true;
        video_ref = refs[k];
        out->subgroup = h;
        out->have_subgroup = true;
    }
    if (!have) return;

    bool     have_prev = false;
    uint64_t prev_object_id = 0;
    for (size_t i = 0; i < t->count; i++) {
        const np_trace_send_t *r = &t->rec[i];
        if (r->stream_ref != video_ref) { out->other_stream_records++; continue; }
        {
            /* The stream-opening header, wherever it sits. */
            np_subgroup_header_t h;
            if (np_read_subgroup_header(enc, r->header, r->header_len, &h))
                continue;
        }

        uint64_t v[NP_HEADER_MAX_INTS];
        if (np_read_int_header(enc, r->header, r->header_len, 2, v) &&
            v[1] == (uint64_t)r->detail_len && !r->detail_truncated) {
            if (out->prop_count >= NP_CLOSURE_MAX_OBJECTS) {
                out->unclassified_count++;
                continue;
            }
            np_captured_prop_t *p = &out->props[out->prop_count++];
            p->object_id = have_prev ? prev_object_id + v[0] + 1 : v[0];
            have_prev = true;
            prev_object_id = p->object_id;
            p->declared_len = v[1];
            p->len = r->detail_len;
            memcpy(p->bytes, r->detail, r->detail_len);
            continue;
        }
        if (np_read_int_header(enc, r->header, r->header_len, 1, v) &&
            v[0] == (uint64_t)r->detail_len && !r->detail_truncated) {
            if (out->payload_count >= NP_CLOSURE_MAX_OBJECTS) {
                out->unclassified_count++;
                continue;
            }
            np_captured_payload_t *p = &out->payloads[out->payload_count++];
            p->declared_len = v[0];
            p->len = r->detail_len;
            memcpy(p->bytes, r->detail, r->detail_len);
            continue;
        }
        out->unclassified_count++;
    }
}

moq_result_t np_closure_run_sender(moq_version_t version,
                                   const np_sender_object_t *objects,
                                   size_t object_count,
                                   np_sender_result_t *out)
{
    if (!out || !objects) return MOQ_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (object_count == 0 || object_count > NP_CLOSURE_MAX_OBJECTS)
        return MOQ_ERR_INVAL;
    np_enc_t enc = np_enc_for_version(version);
    if (enc == (np_enc_t)0) return MOQ_ERR_INVAL;

    static np_trace_ctx_t trace;   /* bounded, reset per run */
    memset(&trace, 0, sizeof(trace));

    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 42;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    cfg.version = version;
    cfg.trace_fn = np_trace_cb;
    cfg.trace_ctx = &trace;

    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) != MOQ_OK || !sp) {
        out->setup_failure = "simpair create";
        return MOQ_OK;
    }
    moq_simpair_start(sp);
    (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);

    moq_session_t *cl = moq_simpair_client(sp);
    moq_session_t *srv = moq_simpair_server(sp);
    np_drain_events(cl);
    np_drain_events(srv);

    moq_media_sender_t *s = NULL;
    moq_subscription_t sub;
    memset(&sub, 0, sizeof(sub));

    if (moq_session_state(cl) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(srv) != MOQ_SESS_ESTABLISHED) {
        out->setup_failure = "handshake did not establish";
        goto done;
    }

    {
        moq_bytes_t ns_parts[2];
        ns_parts[0] = np_lit("svc");
        ns_parts[1] = np_lit("demo");
        moq_media_sender_cfg_t scfg;
        moq_media_sender_cfg_init_live_sized(&scfg, sizeof(scfg));
        scfg.namespace_.parts = ns_parts;
        scfg.namespace_.count = 2;
        scfg.publish_tracks = false;
        /* Disable the periodic catalog refresh: it would put a second data
         * stream on the wire for no reason here. */
        scfg.catalog_refresh_interval_us = UINT64_MAX;
        s = moq_media_sender_test_new_cfg(&scfg);
        if (!s) { out->setup_failure = "sender create"; goto done; }

        moq_media_track_cfg_t tc;
        moq_media_track_cfg_init(&tc);
        tc.name = np_lit(NP_TRACK_NAME);
        tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
        tc.packaging = MOQ_MEDIA_PACKAGING_RAW;
        tc.codec = np_lit("av01");
        tc.bitrate = 1500000;
        tc.is_live = true;
        moq_media_track_t *track = NULL;
        if (moq_media_sender_add_track(s, &tc, &track) != MOQ_OK || !track) {
            out->setup_failure = "add_track";
            goto done;
        }

        uint64_t now = moq_simpair_now_us(sp);

        /* Ready: the peer accepts the namespace. */
        for (int c = 0; c < 16 && !moq_media_sender_is_ready(s); c++) {
            now += NP_PUMP_TICK_US;
            moq_media_sender_test_pump(s, cl, now);
            (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);
            moq_event_t ev;
            while (moq_session_poll_events(srv, &ev, 1) == 1) {
                if (ev.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) {
                    moq_accept_namespace_cfg_t ac;
                    moq_accept_namespace_cfg_init(&ac);
                    (void)moq_session_accept_namespace(
                        srv, ev.u.namespace_published.ann, &ac, now);
                }
                moq_event_cleanup(&ev);
            }
            (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);
        }
        if (!moq_media_sender_is_ready(s)) {
            out->setup_failure = "sender never became ready";
            goto done;
        }

        /* Real demand for the media track itself. */
        moq_bytes_t sub_ns[2];
        sub_ns[0] = np_lit("svc");
        sub_ns[1] = np_lit("demo");
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace.parts = sub_ns;
        sc.track_namespace.count = 2;
        sc.track_name = np_lit(NP_TRACK_NAME);
        sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
        sc.has_forward = true;
        sc.forward = true;
        if (moq_session_subscribe(srv, &sc, now, &sub) != MOQ_OK) {
            out->setup_failure = "peer subscribe";
            goto done;
        }
        for (int c = 0; c < 24 && !out->have_track_alias; c++) {
            now += NP_PUMP_TICK_US;
            moq_media_sender_test_pump(s, cl, now);
            (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);
            moq_event_t ev;
            while (moq_session_poll_events(srv, &ev, 1) == 1) {
                if (ev.kind == MOQ_EVENT_SUBSCRIBE_OK) {
                    out->track_alias = ev.u.subscribe_ok.track_alias;
                    out->have_track_alias = true;
                }
                moq_event_cleanup(&ev);
            }
        }
        if (!out->have_track_alias) {
            out->setup_failure = "peer subscription never accepted";
            goto done;
        }

        /* The negotiated draft is fixed and readable BEFORE object 1. */
        out->version_before_first_write = moq_session_version(cl);
        out->state_before_first_write = moq_session_state(cl);
        out->setup_ok = true;

        for (size_t i = 0; i < object_count; i++) {
            const np_sender_object_t *o = &objects[i];
            size_t plen = o->payload ? strlen(o->payload) : 0;
            moq_rcbuf_t *payload = NULL;
            if (moq_rcbuf_create(moq_alloc_default(),
                                 (const uint8_t *)o->payload, plen,
                                 &payload) != MOQ_OK) {
                out->setup_failure = "payload rcbuf";
                break;
            }
            moq_media_send_object_t so;
            memset(&so, 0, sizeof(so));
            so.struct_size = sizeof(so);
            so.payload = payload;
            so.properties = NULL;      /* RAW: the service owns the block */
            so.is_sync = o->is_sync;
            so.starts_group = o->starts_group;
            so.presentation_time_us = o->capture_time_us;
            so.has_capture_time = true;
            so.capture_time_us = o->capture_time_us;
            moq_result_t wrc = moq_media_sender_write(s, track, &so);
            out->write_rc[out->write_count++] = wrc;
            if (wrc != MOQ_OK)
                moq_rcbuf_decref(payload);   /* no transfer on a non-OK return */

            for (int c = 0; c < 4; c++) {
                now += NP_PUMP_TICK_US;
                moq_media_sender_test_pump(s, cl, now);
                (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS,
                                                      NULL);
                np_drain_events(srv);
            }
        }

        for (int c = 0; c < 8; c++) {
            now += NP_PUMP_TICK_US;
            moq_media_sender_test_pump(s, cl, now);
            (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);
            np_drain_events(srv);
        }

        moq_media_sender_stats_t st;
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        (void)moq_media_sender_get_stats(s, &st, sizeof(st));
        out->objects_written = st.objects_written;
        out->objects_sent = st.objects_sent;
        out->objects_dropped = st.objects_dropped;
        out->groups_dropped = st.groups_dropped;
        out->groups_abandoned = st.groups_abandoned;
        out->sender_last_error = st.last_error;
        out->sender_fatal = moq_media_sender_is_fatal(s);
        out->sender_fatal_code = moq_media_sender_fatal_code(s);
        out->sender_closed = moq_media_sender_is_closed(s);
        out->session_state_after = moq_session_state(cl);

        out->trace_overflow = trace.overflow;
        out->send_data_records = trace.count;
        np_classify_sender_trace(&trace, enc, out);
    }

done:
    if (s) moq_media_sender_test_free(s);
    np_drain_events(cl);
    np_drain_events(srv);
    moq_simpair_destroy(sp);
    return MOQ_OK;
}

/* ===================================================================== *
 *  Direction B
 * ===================================================================== */

/* Drain the receiver's track events. Discovery and the coalesced parse-drop
 * diagnostic are both recorded: the drop CLASS and its reported totals are
 * what make the accounting an observation rather than a bare counter. */
static void np_drain_track_events(moq_media_receiver_t *r,
                                  np_receiver_result_t *out)
{
    moq_media_track_event_t te;
    while (moq_media_receiver_poll_track(r, &te, sizeof(te)) == MOQ_OK) {
        if (te.kind == MOQ_MEDIA_TRACK_ADDED) {
            out->track_added = true;
            out->track_added_count++;
        } else if (te.kind == MOQ_MEDIA_CATALOG_READY) {
            out->catalog_ready = true;
            out->catalog_ready_count++;
        } else if (te.kind == MOQ_MEDIA_TRACK_PARSE_DROP) {
            out->parse_drop_events++;
            if (te.struct_size >= offsetof(moq_media_track_event_t,
                                           parse_drops_total) +
                                  sizeof(te.parse_drops_total)) {
                out->parse_drop_last_class = te.parse_drop_class;
                out->parse_drops_reported_total = te.parse_drops_total;
            }
        } else {
            out->other_track_events++;
        }
    }
}

/* Is this the fixture's own namespace, part for part? */
static bool np_ns_is_fixture(const moq_namespace_t *ns)
{
    static const char *want[2] = { "svc", "demo" };
    if (!ns || ns->count != 2 || !ns->parts) return false;
    for (size_t i = 0; i < 2; i++) {
        size_t n = strlen(want[i]);
        if (ns->parts[i].len != n || !ns->parts[i].data) return false;
        if (memcmp(ns->parts[i].data, want[i], n) != 0) return false;
    }
    return true;
}

static bool np_name_is(const moq_bytes_t *b, const char *want)
{
    size_t n = strlen(want);
    return b && b->len == n && b->data && memcmp(b->data, want, n) == 0;
}

/* One live LOC video track, and nothing else: the catalog the receiver must
 * accept before it will route a media object. */
static const char NP_CATALOG_JSON[] =
    "{\"version\":1,\"tracks\":["
    "{\"name\":\"" NP_TRACK_NAME "\",\"packaging\":\"loc\",\"isLive\":true,"
    "\"role\":\"video\",\"codec\":\"av01\",\"bitrate\":1500000}]}";

#define NP_ALIAS_CATALOG 0u
#define NP_ALIAS_VIDEO   2u

moq_result_t np_closure_run_receiver(moq_version_t version,
                                     const np_recv_input_t *inputs,
                                     size_t input_count,
                                     np_receiver_result_t *out)
{
    if (!out || !inputs) return MOQ_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (input_count == 0 || input_count > NP_CLOSURE_MAX_OBJECTS)
        return MOQ_ERR_INVAL;
    if (np_enc_for_version(version) == (np_enc_t)0) return MOQ_ERR_INVAL;

    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default();
    cfg.seed = 42;
    cfg.initial_now_us = 1000;
    cfg.client_send_request_capacity = true;
    cfg.client_initial_request_capacity = 32;
    cfg.server_send_request_capacity = true;
    cfg.server_initial_request_capacity = 32;
    cfg.version = version;

    moq_simpair_t *sp = NULL;
    if (moq_simpair_create(&cfg, &sp) != MOQ_OK || !sp) {
        out->setup_failure = "simpair create";
        return MOQ_OK;
    }
    moq_simpair_start(sp);
    (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);

    moq_session_t *cl = moq_simpair_client(sp);
    moq_session_t *srv = moq_simpair_server(sp);
    np_drain_events(cl);
    np_drain_events(srv);

    moq_media_receiver_t *r = NULL;

    if (moq_session_state(cl) != MOQ_SESS_ESTABLISHED ||
        moq_session_state(srv) != MOQ_SESS_ESTABLISHED) {
        out->setup_failure = "handshake did not establish";
        goto done;
    }

    {
        moq_bytes_t ns_parts[2];
        ns_parts[0] = np_lit("svc");
        ns_parts[1] = np_lit("demo");
        moq_media_receiver_cfg_t rcfg;
        moq_media_receiver_cfg_init_live(&rcfg);
        rcfg.namespace_.parts = ns_parts;
        rcfg.namespace_.count = 2;
        rcfg.auto_subscribe = true;
        /* RAW: the reported capture timestamp must be the wire value, not a
         * value rebased against a receiver-wide epoch. */
        rcfg.time_mode = MOQ_MEDIA_TIME_RAW;
        r = moq_media_receiver_test_new_cfg(&rcfg);
        if (!r) { out->setup_failure = "receiver create"; goto done; }

        moq_subscription_t cat_sub;
        moq_subscription_t vid_sub;
        memset(&cat_sub, 0, sizeof(cat_sub));
        memset(&vid_sub, 0, sizeof(vid_sub));
        bool have_cat_sub = false, have_vid_sub = false;
        bool catalog_published = false;
        size_t video_written = 0;
        uint64_t now = moq_simpair_now_us(sp);
        bool version_read = false;

        for (int cycle = 0; cycle < 60; cycle++) {
            now += NP_PUMP_TICK_US;
            moq_media_receiver_test_pump(r, cl, now);
            (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);

            moq_event_t ev;
            while (moq_session_poll_events(srv, &ev, 1) == 1) {
                if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) {
                    /* Classify EXACTLY -- namespace and name both -- rather
                     * than treating whatever is not the media track as the
                     * catalog. An unexpected request is recorded, never
                     * accepted, so a duplicate or a foreign name cannot be
                     * absorbed into a correct-looking run. */
                    const moq_subscribe_request_event_t *sr =
                        &ev.u.subscribe_request;
                    bool ns_ok = np_ns_is_fixture(&sr->track_namespace);
                    bool is_video = ns_ok && np_name_is(&sr->track_name,
                                                        NP_TRACK_NAME);
                    bool is_catalog = ns_ok && np_name_is(&sr->track_name,
                                                          NP_CATALOG_NAME);
                    /* The name is BORROWED from output scratch, which the
                     * accept below invalidates (it is an advancing call). Copy
                     * it here, before anything advances, so the diagnostic
                     * cannot report freed scratch. */
                    char nm[sizeof(out->sub_unexpected_name)];
                    {
                        size_t n = sr->track_name.len;
                        if (n > sizeof(nm) - 1) n = sizeof(nm) - 1;
                        if (sr->track_name.data && n)
                            memcpy(nm, sr->track_name.data, n);
                        nm[n] = '\0';
                    }
                    if (is_video && !have_vid_sub) {
                        out->sub_video_count++;
                        moq_accept_subscribe_cfg_t ac;
                        memset(&ac, 0, sizeof(ac));
                        ac.struct_size = sizeof(ac);
                        ac.has_track_alias = true;
                        ac.track_alias = NP_ALIAS_VIDEO;
                        out->accept_video_rc = moq_session_accept_subscribe(
                            srv, sr->sub, &ac, now);
                        if (out->accept_video_rc == MOQ_OK) {
                            vid_sub = sr->sub;
                            have_vid_sub = true;
                        }
                    } else if (is_catalog && !have_cat_sub) {
                        out->sub_catalog_count++;
                        moq_accept_subscribe_cfg_t ac;
                        memset(&ac, 0, sizeof(ac));
                        ac.struct_size = sizeof(ac);
                        ac.has_track_alias = true;
                        ac.track_alias = NP_ALIAS_CATALOG;
                        out->accept_catalog_rc = moq_session_accept_subscribe(
                            srv, sr->sub, &ac, now);
                        if (out->accept_catalog_rc == MOQ_OK) {
                            cat_sub = sr->sub;
                            have_cat_sub = true;
                        }
                    } else {
                        if (is_video) out->sub_video_count++;
                        else if (is_catalog) out->sub_catalog_count++;
                        out->sub_unexpected_count++;
                        if (out->sub_unexpected_name[0] == '\0')
                            memcpy(out->sub_unexpected_name, nm, sizeof(nm));
                    }
                } else {
                    out->other_session_events++;
                    out->other_session_event_kind = (uint32_t)ev.kind;
                }
                moq_event_cleanup(&ev);
            }
            (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);

            /* The catalog: one object on its own subgroup, no properties. */
            if (have_cat_sub && !catalog_published) {
                moq_subgroup_cfg_t gc;
                memset(&gc, 0, sizeof(gc));
                gc.struct_size = sizeof(gc);
                gc.group_id = 0;
                gc.subgroup_id = 0;
                gc.publisher_priority = 128;
                gc.object_properties = false;
                gc.end_of_group = true;
                moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
                if (moq_session_open_subgroup(srv, cat_sub, &gc, now, &sg)
                    == MOQ_OK) {
                    moq_rcbuf_t *payload = NULL;
                    if (moq_rcbuf_create(moq_alloc_default(),
                                         (const uint8_t *)NP_CATALOG_JSON,
                                         sizeof(NP_CATALOG_JSON) - 1,
                                         &payload) == MOQ_OK) {
                        if (moq_session_write_object(srv, sg, 0, payload, now)
                            == MOQ_OK)
                            catalog_published = true;
                        moq_rcbuf_decref(payload);
                    }
                    (void)moq_session_close_subgroup(srv, sg, now);
                }
                (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS,
                                                      NULL);
            }

            /* The media objects: one subgroup carrying object properties,
             * whose property bytes come from role 2 and nowhere else. */
            if (have_vid_sub && catalog_published && video_written == 0) {
                if (!version_read) {
                    out->version_before_first_write = moq_session_version(srv);
                    out->state_before_first_write = moq_session_state(srv);
                    version_read = true;
                }
                moq_subgroup_cfg_t gc;
                memset(&gc, 0, sizeof(gc));
                gc.struct_size = sizeof(gc);
                gc.group_id = 0;
                gc.subgroup_id = 0;
                gc.publisher_priority = 128;
                gc.object_properties = true;
                gc.end_of_group = true;
                moq_subgroup_handle_t sg = MOQ_SUBGROUP_INVALID;
                if (moq_session_open_subgroup(srv, vid_sub, &gc, now, &sg)
                    == MOQ_OK) {
                    for (size_t i = 0; i < input_count; i++) {
                        moq_rcbuf_t *props = NULL;
                        moq_rcbuf_t *payload = NULL;
                        size_t plen = inputs[i].payload
                            ? strlen(inputs[i].payload) : 0;
                        if (moq_rcbuf_create(moq_alloc_default(),
                                             inputs[i].properties,
                                             inputs[i].properties_len,
                                             &props) != MOQ_OK) break;
                        if (moq_rcbuf_create(moq_alloc_default(),
                                             (const uint8_t *)inputs[i].payload,
                                             plen, &payload) != MOQ_OK) {
                            moq_rcbuf_decref(props);
                            break;
                        }
                        moq_object_cfg_t oc;
                        moq_object_cfg_init(&oc);
                        oc.object_id = (uint64_t)i;
                        oc.payload = payload;
                        oc.properties = props;
                        moq_result_t wrc =
                            moq_session_write_object_ex(srv, sg, &oc, now);
                        out->publish_rc[i] = wrc;
                        if (wrc == MOQ_OK) out->published_count++;
                        video_written++;
                        moq_rcbuf_decref(props);
                        moq_rcbuf_decref(payload);
                    }
                    (void)moq_session_close_subgroup(srv, sg, now);
                }
                (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS,
                                                      NULL);
            }

            np_drain_track_events(r, out);

            (void)moq_session_process_pending(cl, now);
            if (moq_media_receiver_is_fatal(r)) break;
        }

        if (!have_cat_sub) out->setup_failure = "catalog subscribe never seen";
        else if (!catalog_published) out->setup_failure = "catalog not published";
        else if (!have_vid_sub) out->setup_failure = "media subscribe never seen";
        else if (video_written != input_count)
            out->setup_failure = "media objects not all attempted";
        else out->setup_ok = true;

        /* A few more pumps so queued objects route through the real hook. */
        for (int c = 0; c < 8; c++) {
            now += NP_PUMP_TICK_US;
            moq_media_receiver_test_pump(r, cl, now);
            (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);
            np_drain_track_events(r, out);
        }

        moq_media_object_t obj;
        while (moq_media_receiver_poll_object(r, &obj, sizeof(obj)) == MOQ_OK) {
            if (out->object_count < NP_CLOSURE_MAX_OBJECTS) {
                np_recv_object_t *o = &out->objects[out->object_count++];
                o->has_capture_time = obj.has_capture_time;
                o->capture_time_us = obj.capture_time_us;
                o->keyframe = obj.keyframe;
                o->len = obj.payload.len <= sizeof(o->bytes)
                    ? obj.payload.len : sizeof(o->bytes);
                if (obj.payload.data && o->len)
                    memcpy(o->bytes, obj.payload.data, o->len);
            }
            moq_media_object_cleanup(&obj);
        }

        moq_media_receiver_stats_t st;
        memset(&st, 0, sizeof(st));
        st.struct_size = sizeof(st);
        (void)moq_media_receiver_get_stats(r, &st, sizeof(st));
        out->objects_received = st.objects_received;
        out->objects_dropped = st.objects_dropped;
        out->parse_drops = st.parse_drops;
        out->catalog_drops = st.catalog_drops;
        out->receiver_fatal = moq_media_receiver_is_fatal(r);
        out->receiver_fatal_code = moq_media_receiver_fatal_code(r);
        out->session_state_after = moq_session_state(cl);
    }

done:
    /* Retire the receiver through the PRODUCTION teardown path, while the
     * client session is still live and the pair is quiescent so the per-handle
     * UNSUBSCRIBE has action capacity and cannot be refused. Then deliver that
     * teardown traffic and drain both sides before the pair goes away. Valid on
     * every exit, including the early ones where no subscription was ever
     * established. */
    if (r) {
        (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);
        moq_media_receiver_test_destroy_with_session(
            r, cl, moq_simpair_now_us(sp) + NP_PUMP_TICK_US);
        (void)moq_simpair_run_until_quiescent(sp, NP_QUIESCE_STEPS, NULL);
    }
    np_drain_events(cl);
    np_drain_events(srv);
    moq_simpair_destroy(sp);
    return MOQ_OK;
}
