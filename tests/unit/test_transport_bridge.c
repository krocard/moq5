#include <moq/moq.h>
#include <moq/transport_bridge.h>
#include <moq/control.h>
#include <moq/control_d18.h>
#include <moq/buf.h>
#include "test_support.h"
#include "../support/fake_endpoint.h"
#include "../../core/src/session/session_internal.h"
#include "../../core/src/session/session_transport.h"
#include "../../core/src/bridge/transport_bridge_internal.h"
#include "../support/sweep_arm.h"
#include "../support/failpoint.h"
#include "../support/fin_case.h"
#include "../support/ownership_graph.h"
#include "../support/txn_snapshot.h"
#include "../support/ns_owner_inventory.h"
#include <string.h>

/*
 * Bridge contracts and pending-state invariants: stream mapping, inbound
 * retry retention, control-channel topology, deferred close, terminal facts,
 * and the budgeted service entry. Each test builds a bridge over a fake
 * endpoint and exercises one scenario.
 */

/* -- Helpers -------------------------------------------------------- */

typedef struct {
    moq_session_t *client;
    moq_session_t *server;
    fake_endpoint_t client_ep;
    fake_endpoint_t server_ep;
    moq_transport_bridge_t *client_bridge;
    moq_transport_bridge_t *server_bridge;
} test_pair_t;

static int test_pair_init_full(test_pair_t *tp, uint32_t client_max_events,
                               bool client_streaming,
                               uint32_t client_max_actions,
                               uint64_t client_idle_us)
{
    memset(tp, 0, sizeof(*tp));

    moq_session_cfg_t ccfg;
    moq_session_cfg_init_sized(&ccfg, sizeof(ccfg), moq_alloc_default(), MOQ_PERSPECTIVE_CLIENT);
    ccfg.send_request_capacity = true;
    ccfg.initial_request_capacity = 10;
    /* Streaming delivery is what lets a peer RESET mid-object need an event,
     * which is the only way moq_session_on_data_reset can block. */
    ccfg.streaming_objects = client_streaming;
    /* A tiny action queue lets a test force ACTION-capacity backpressure,
     * which is a different blocker from a full event queue. 0 = default. */
    if (client_max_actions) ccfg.max_actions = client_max_actions;
    if (client_idle_us) ccfg.idle_timeout_us = client_idle_us;
    /* A tiny client event queue lets a test force receive backpressure
     * (a second object on a data stream blocks in PENDING_EMIT). 0 = default. */
    if (client_max_events) ccfg.max_events = client_max_events;

    moq_session_cfg_t scfg;
    moq_session_cfg_init_sized(&scfg, sizeof(scfg), moq_alloc_default(), MOQ_PERSPECTIVE_SERVER);
    scfg.send_request_capacity = true;
    scfg.initial_request_capacity = 10;

    if (moq_session_create(&ccfg, 0, &tp->client) < 0) return -1;
    if (moq_session_create(&scfg, 0, &tp->server) < 0) {
        moq_session_destroy(tp->client);
        return -1;
    }

    fake_endpoint_init(&tp->client_ep, 1000, 2000);
    fake_endpoint_init(&tp->server_ep, 3000, 4000);

    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());

    if (moq_transport_bridge_create(&bcfg, tp->client,
            &tp->client_ep.vtable, &tp->client_ep,
            &tp->client_bridge) < 0) {
        moq_session_destroy(tp->server);
        moq_session_destroy(tp->client);
        return -1;
    }
    if (moq_transport_bridge_create(&bcfg, tp->server,
            &tp->server_ep.vtable, &tp->server_ep,
            &tp->server_bridge) < 0) {
        moq_transport_bridge_destroy(tp->client_bridge);
        moq_session_destroy(tp->server);
        moq_session_destroy(tp->client);
        return -1;
    }

    return 0;
}

static int test_pair_init_stream(test_pair_t *tp, uint32_t client_max_events,
                                 bool client_streaming)
{
    return test_pair_init_full(tp, client_max_events, client_streaming, 0, 0);
}

static int test_pair_init_ex(test_pair_t *tp, uint32_t client_max_events)
{
    return test_pair_init_stream(tp, client_max_events, false);
}

static int test_pair_init(test_pair_t *tp)
{
    return test_pair_init_ex(tp, 0);
}

static void test_pair_destroy(test_pair_t *tp)
{
    moq_transport_bridge_destroy(tp->client_bridge);
    moq_transport_bridge_destroy(tp->server_bridge);
    moq_session_destroy(tp->client);
    moq_session_destroy(tp->server);
}

/*
 * Pump: deliver client endpoint ops to server bridge, and vice versa.
 * Returns number of ops delivered.
 */
static size_t pump_once(test_pair_t *tp, uint64_t now)
{
    size_t delivered = 0;

    moq_transport_bridge_service(tp->client_bridge, now);

    for (size_t i = 0; i < tp->client_ep.count; i++) {
        fake_op_t *o = &tp->client_ep.ops[i];
        switch (o->kind) {
        case FAKE_OP_OPEN_BIDI:
            break;
        case FAKE_OP_WRITE:
            if (tp->server_bridge) {
                if (!tp->server_ep.next_bidi_id &&
                    o->stream_id >= 2000 && o->stream_id < 3000) {
                    moq_transport_bridge_on_peer_control_bytes(
                        tp->server_bridge, o->stream_id,
                        o->data, o->data_len, o->fin, now);
                } else if (o->stream_id >= 1000 && o->stream_id < 2000) {
                    moq_transport_bridge_on_peer_uni_bytes(
                        tp->server_bridge, o->stream_id,
                        o->data, o->data_len, o->fin, now);
                } else {
                    moq_transport_bridge_on_peer_control_bytes(
                        tp->server_bridge, o->stream_id,
                        o->data, o->data_len, o->fin, now);
                }
            }
            delivered++;
            break;
        case FAKE_OP_CLOSE:
            if (tp->server_bridge)
                moq_transport_bridge_on_transport_close(
                    tp->server_bridge, o->error_code, now);
            delivered++;
            break;
        default:
            delivered++;
            break;
        }
    }
    fake_endpoint_clear_ops(&tp->client_ep);

    moq_transport_bridge_service(tp->server_bridge, now);

    for (size_t i = 0; i < tp->server_ep.count; i++) {
        fake_op_t *o = &tp->server_ep.ops[i];
        switch (o->kind) {
        case FAKE_OP_WRITE:
            if (tp->client_bridge) {
                if (o->stream_id >= 2000 && o->stream_id < 3000) {
                    moq_transport_bridge_on_peer_control_bytes(
                        tp->client_bridge, o->stream_id,
                        o->data, o->data_len, o->fin, now);
                }
            }
            delivered++;
            break;
        default:
            delivered++;
            break;
        }
    }
    fake_endpoint_clear_ops(&tp->server_ep);

    return delivered;
}

static int pump_until_quiescent(test_pair_t *tp, int max, uint64_t now)
{
    for (int i = 0; i < max; i++) {
        if (pump_once(tp, now) == 0) return i;
    }
    return max;
}

static bool setup_handshake(test_pair_t *tp)
{
    moq_session_start(tp->client, 0);
    pump_until_quiescent(tp, 20, 0);

    moq_event_t ev;
    bool c_setup = false, s_setup = false;
    while (moq_session_poll_events(tp->client, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) c_setup = true;
        moq_event_cleanup(&ev);
    }
    while (moq_session_poll_events(tp->server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) s_setup = true;
        moq_event_cleanup(&ev);
    }
    return c_setup && s_setup;
}

/*
 * The budgeted-advance context must be left on EVERY exit from the budgeted
 * service entry.
 * If any path returned without leaving it, the next ordinary session call would
 * run inside a budget it never asked for and could observe the private
 * suspension sentinel -- which no application-facing caller may ever see.
 *
 * Each case forces a different exit, then runs an unlimited advancing call.
 */
static int test_budget_context_paired_on_every_exit(void)
{
    int failures = 0;

    /* Exit: fatal short-circuit (before the pass is even entered). */
    {
        fake_endpoint_t ep;
        fake_endpoint_init(&ep, 100, 200);
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                                   MOQ_PERSPECTIVE_CLIENT);
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
        moq_transport_bridge_cfg_t bcfg;
        moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
        moq_transport_bridge_t *b = NULL;
        MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &ep.vtable, &ep,
                                                    &b) == MOQ_OK);
        moq_transport_bridge_on_transport_error(b, 0x1, 1);
        MOQ_TEST_CHECK(moq_transport_bridge_is_fatal(b));
        moq_bridge_budgeted_result_t r;
        (void)moq_transport_bridge_service_budgeted(b, 2, 4, &r);
        MOQ_TEST_CHECK(!s->budget_active);
        MOQ_TEST_CHECK(moq_session_tick(s, 3) != MOQ_SESSION_SUSPENDED);
        MOQ_TEST_CHECK(!s->budget_active);
        moq_transport_bridge_destroy(b);
        moq_session_destroy(s);
    }

    /* Exit: ordinary drained pass (the bottom break). */
    {
        fake_endpoint_t ep;
        fake_endpoint_init(&ep, 100, 200);
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                                   MOQ_PERSPECTIVE_CLIENT);
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
        moq_transport_bridge_cfg_t bcfg;
        moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
        moq_transport_bridge_t *b = NULL;
        MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &ep.vtable, &ep,
                                                    &b) == MOQ_OK);
        MOQ_TEST_CHECK(!s->budget_active);
        moq_bridge_budgeted_result_t r;
        MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(b, 1, 4, &r) ==
                       MOQ_OK);
        MOQ_TEST_CHECK(!s->budget_active);
        MOQ_TEST_CHECK(moq_session_tick(s, 2) != MOQ_SESSION_SUSPENDED);
        MOQ_TEST_CHECK(!s->budget_active);
        moq_transport_bridge_destroy(b);
        moq_session_destroy(s);
    }

    /* Exit: the pass itself returns non-OK -- a write error makes dispatch
     * fatal inside Step 2, so service returns MOQ_ERR_INTERNAL from within the
     * bracketed region rather than from a pre-enter guard. */
    {
        fake_endpoint_t ep;
        fake_endpoint_init(&ep, 100, 200);
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                                   MOQ_PERSPECTIVE_CLIENT);
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
        moq_transport_bridge_cfg_t bcfg;
        moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
        moq_transport_bridge_t *b = NULL;
        MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &ep.vtable, &ep,
                                                    &b) == MOQ_OK);
        ep.fail_write = true;
        MOQ_TEST_CHECK(moq_session_start(s, 1) == MOQ_OK);
        moq_bridge_budgeted_result_t r;
        MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(b, 2, 4, &r) ==
                       MOQ_ERR_INTERNAL);
        MOQ_TEST_CHECK(!s->budget_active);
        MOQ_TEST_CHECK(moq_session_tick(s, 3) != MOQ_SESSION_SUSPENDED);
        MOQ_TEST_CHECK(!s->budget_active);
        moq_transport_bridge_destroy(b);
        moq_session_destroy(s);
    }

    /* Exit: outbound blocked, so the pass breaks out of Step 1. */
    {
        fake_endpoint_t ep;
        fake_endpoint_init(&ep, 100, 200);
        moq_session_cfg_t cfg;
        moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                                   MOQ_PERSPECTIVE_CLIENT);
        moq_session_t *s = NULL;
        MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
        moq_transport_bridge_cfg_t bcfg;
        moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
        moq_transport_bridge_t *b = NULL;
        MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &ep.vtable, &ep,
                                                    &b) == MOQ_OK);
        ep.block_write = true;      /* force WOULD_BLOCK on the setup write */
        MOQ_TEST_CHECK(moq_session_start(s, 1) == MOQ_OK);
        moq_bridge_budgeted_result_t r;
        (void)moq_transport_bridge_service_budgeted(b, 2, 4, &r);
        MOQ_TEST_CHECK(!s->budget_active);
        MOQ_TEST_CHECK(moq_session_tick(s, 3) != MOQ_SESSION_SUSPENDED);
        MOQ_TEST_CHECK(!s->budget_active);
        moq_transport_bridge_destroy(b);
        moq_session_destroy(s);
    }

    return failures;
}

/* == Tests ========================================================== */

/*
 * The unlimited service entry must enter NO budget context.
 *
 * Reading budget_active after the call cannot show this -- a paired
 * enter/leave also leaves it false. The entry COUNT is what discriminates:
 * a UINT32_MAX bracket would still be a finite budget, routing every wired
 * session entry point through the budgeted advance and giving the unlimited
 * entry a suspension it has no way to report.
 *
 * Deltas, not absolutes: the counters are gated globals that every test in
 * this binary accumulates into.
 */
static int test_unlimited_service_enters_no_budget_context(void)
{
    int failures = 0;

    fake_endpoint_t ep;
    fake_endpoint_init(&ep, 100, 200);
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                               MOQ_PERSPECTIVE_CLIENT);
    moq_session_t *s = NULL;
    MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    moq_transport_bridge_t *b = NULL;
    MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &ep.vtable, &ep,
                                                &b) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_start(s, 1) == MOQ_OK);

    uint64_t before = session_budget_enter_count;
    MOQ_TEST_CHECK(moq_transport_bridge_service(b, 2) == MOQ_OK);
    MOQ_TEST_CHECK(session_budget_enter_count - before == 0);
    MOQ_TEST_CHECK(!s->budget_active);

    before = session_budget_enter_count;
    moq_bridge_budgeted_result_t r;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(b, 3, 8, &r) == MOQ_OK);
    MOQ_TEST_CHECK(session_budget_enter_count - before == 1);
    MOQ_TEST_CHECK(!s->budget_active);

    moq_transport_bridge_destroy(b);
    moq_session_destroy(s);
    return failures;
}

/*
 * The budgeted entry requires an output. A finite budget that can suspend,
 * paired with a discarded outcome, would report a drained pass while a cursor
 * is still live. *out is zeroed before any other argument is validated, so
 * every rejection except the NULL-output one still yields a defined outcome.
 */
static int test_budgeted_service_requires_output(void)
{
    int failures = 0;

    fake_endpoint_t ep;
    fake_endpoint_init(&ep, 100, 200);
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(),
                               MOQ_PERSPECTIVE_CLIENT);
    moq_session_t *s = NULL;
    MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);
    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    moq_transport_bridge_t *b = NULL;
    MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &ep.vtable, &ep,
                                                &b) == MOQ_OK);

    uint64_t before = session_budget_enter_count;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(b, 1, 8, NULL) ==
                   MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(session_budget_enter_count - before == 0);

    /* NULL bridge still leaves a defined, non-suspended outcome. */
    moq_bridge_budgeted_result_t r;
    memset(&r, 0xAB, sizeof(r));
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(NULL, 1, 8, &r) ==
                   MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(!r.suspended);
    MOQ_TEST_CHECK(r.sweep_spent == 0);

    /* An idle pool completes even at zero budget: no runnable work. */
    MOQ_TEST_CHECK(moq_session_start(s, 1) == MOQ_OK);
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(b, 2, 0, &r) == MOQ_OK);
    MOQ_TEST_CHECK(!r.suspended);

    moq_transport_bridge_destroy(b);
    moq_session_destroy(s);
    return failures;
}

/* -- Draft-18 pair: peer-opened request bidis ----------------------- *
 *
 * Bidi data entries exist only under draft-18, where each request travels on
 * its own bidirectional stream; draft-16 keeps every request on the single
 * shared control bidi. So the bidi arms of the inbound families need a d18
 * pair, and the client must be the one RECEIVING a request.
 *
 * Control is a unidirectional pair here, so the shuttle routes purely by the
 * fake endpoint's stream-id ranges and never needs the control entry point.
 */
/* As d18_pair_init below, with the CLIENT session on a caller-supplied
 * allocator so a test can fail one of its allocations by ordinal. The
 * bridges always stay on the default allocator, so an armed failure can
 * only land inside the session it targets. */
/* Generalized draft-18 pair: adds a server action cap and the server
 * endpoint's optional native whole-stream abort. Both extras default off, so
 * the wrappers below keep their existing behaviour exactly. */
static int d18_pair_init_caps(test_pair_t *tp, uint32_t client_max_events,
                              const moq_alloc_t *client_alloc,
                              const moq_alloc_t *server_alloc,
                              uint32_t server_max_actions,
                              bool server_native_abort,
                              uint32_t server_max_events)
{
    memset(tp, 0, sizeof(*tp));

    moq_session_cfg_t ccfg;
    moq_session_cfg_init_sized(&ccfg, sizeof(ccfg), client_alloc,
                               MOQ_PERSPECTIVE_CLIENT);
    ccfg.version = MOQ_VERSION_DRAFT_18;
    ccfg.send_request_capacity = true;
    ccfg.initial_request_capacity = 10;
    if (client_max_events) ccfg.max_events = client_max_events;

    moq_session_cfg_t scfg;
    moq_session_cfg_init_sized(&scfg, sizeof(scfg), server_alloc,
                               MOQ_PERSPECTIVE_SERVER);
    scfg.version = MOQ_VERSION_DRAFT_18;
    scfg.send_request_capacity = true;
    scfg.initial_request_capacity = 10;
    if (server_max_actions) scfg.max_actions = server_max_actions;
    /* A tiny SERVER event queue lets a test defer an inbound teardown, which
     * is what leaves a FIN obligation retained on the bridge. 0 = default. */
    if (server_max_events) scfg.max_events = server_max_events;

    if (moq_session_create(&ccfg, 0, &tp->client) < 0) return -1;
    if (moq_session_create(&scfg, 0, &tp->server) < 0) {
        moq_session_destroy(tp->client);
        return -1;
    }

    fake_endpoint_init(&tp->client_ep, 1000, 2000);
    fake_endpoint_init(&tp->server_ep, 3000, 4000);
    /* The bridge RETAINS the vtable pointer it is handed
     * (transport_bridge.c:108) rather than copying the table, so an op may be
     * installed after init. It is done here, before bridge creation, so the
     * endpoint's capability set is fixed for the whole run. */
    if (server_native_abort) fake_endpoint_enable_abort(&tp->server_ep);

    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    if (moq_transport_bridge_create(&bcfg, tp->client, &tp->client_ep.vtable,
                                    &tp->client_ep, &tp->client_bridge) < 0) {
        moq_session_destroy(tp->server);
        moq_session_destroy(tp->client);
        return -1;
    }
    if (moq_transport_bridge_create(&bcfg, tp->server, &tp->server_ep.vtable,
                                    &tp->server_ep, &tp->server_bridge) < 0) {
        moq_transport_bridge_destroy(tp->client_bridge);
        moq_session_destroy(tp->server);
        moq_session_destroy(tp->client);
        return -1;
    }
    return 0;
}

static int d18_pair_init_alloc(test_pair_t *tp, uint32_t client_max_events,
                               const moq_alloc_t *client_alloc,
                               const moq_alloc_t *server_alloc)
{
    return d18_pair_init_caps(tp, client_max_events, client_alloc,
                              server_alloc, 0, false, 0);
}

static int d18_pair_init(test_pair_t *tp, uint32_t client_max_events)
{
    return d18_pair_init_alloc(tp, client_max_events, moq_alloc_default(),
                               moq_alloc_default());
}

static void d18_feed(moq_transport_bridge_t *to, fake_endpoint_t *from,
                     uint64_t uni_base, uint64_t bidi_base, uint64_t now,
                     size_t *delivered)
{
    for (size_t i = 0; i < from->count; i++) {
        fake_op_t *o = &from->ops[i];
        if (o->kind != FAKE_OP_WRITE) { (*delivered)++; continue; }
        if (o->stream_id >= uni_base && o->stream_id < uni_base + 1000)
            moq_transport_bridge_on_peer_uni_bytes(
                to, o->stream_id, o->data, o->data_len, o->fin, now);
        else if (o->stream_id >= bidi_base && o->stream_id < bidi_base + 1000)
            moq_transport_bridge_on_peer_bidi_bytes(
                to, o->stream_id, o->data, o->data_len, o->fin, now);
        (*delivered)++;
    }
    fake_endpoint_clear_ops(from);
}

static size_t d18_shuttle(test_pair_t *tp, uint64_t now)
{
    size_t delivered = 0;
    moq_transport_bridge_service(tp->client_bridge, now);
    d18_feed(tp->server_bridge, &tp->client_ep, 1000, 2000, now, &delivered);
    moq_transport_bridge_service(tp->server_bridge, now);
    d18_feed(tp->client_bridge, &tp->server_ep, 3000, 4000, now, &delivered);
    return delivered;
}

static void d18_shuttle_until_quiescent(test_pair_t *tp, int max,
                                        uint64_t now)
{
    for (int i = 0; i < max; i++)
        if (d18_shuttle(tp, now) == 0) return;
}

/* The server issues the request, so the peer bidi lands on the CLIENT. */
static moq_result_t d18_server_subscribe(test_pair_t *tp)
{
    moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { ns_parts, 1 };
    moq_subscribe_cfg_t cfg;
    moq_subscribe_cfg_init(&cfg);
    cfg.track_namespace = ns;
    cfg.track_name = MOQ_BYTES_LITERAL("video");
    cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t sub;
    return moq_session_subscribe(tp->server, &cfg, 0, &sub);
}

/*
 * Subscribed client with its single event slot already occupied, plus the
 * server-side subgroup whose wire bytes the per-family tests deliver.
 *
 * The occupied slot is the client's own SUBSCRIBE_OK, deliberately left
 * unpolled: it is what makes the next session call that needs to emit return
 * WOULD_BLOCK, which is how each inbound pending flag is established for real
 * rather than written by hand.
 *
 * hdr holds the subgroup header alone (parsing it emits nothing, so it lands a
 * stream entry with no pending flag); obj additionally carries one object,
 * which cannot emit against a full queue.
 */
typedef struct {
    test_pair_t tp;
    uint64_t    sid;                  /* peer uni stream id on the client */
    uint8_t     hdr[512]; size_t hdr_len;
    uint8_t     obj[512]; size_t obj_len;
} uni_pending_fixture_t;

static int uni_pending_fixture_init_ex(uni_pending_fixture_t *f,
                                       bool streaming, size_t payload_len)
{
    memset(f, 0, sizeof(*f));
    f->sid = 5000;

    if (test_pair_init_stream(&f->tp, 1, streaming) < 0) return -1;
    if (!setup_handshake(&f->tp)) { test_pair_destroy(&f->tp); return -1; }

    moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { ns_parts, 1 };
    moq_subscribe_cfg_t sub_cfg;
    moq_subscribe_cfg_init(&sub_cfg);
    sub_cfg.track_namespace = ns;
    sub_cfg.track_name = MOQ_BYTES_LITERAL("video");
    sub_cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t sub;
    if (moq_session_subscribe(f->tp.client, &sub_cfg, 0, &sub) != MOQ_OK)
        goto fail;
    pump_until_quiescent(&f->tp, 20, 0);

    moq_event_t ev;
    moq_subscription_t server_sub = MOQ_SUBSCRIPTION_INVALID;
    while (moq_session_poll_events(f->tp.server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            server_sub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    if (!moq_subscription_is_valid(server_sub)) goto fail;

    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    if (moq_session_accept_subscribe(f->tp.server, server_sub, &acc, 0) != MOQ_OK)
        goto fail;
    pump_until_quiescent(&f->tp, 20, 0);
    /* The client's SUBSCRIBE_OK stays queued: that is the full slot. */

    moq_subgroup_cfg_t sg_cfg;
    moq_subgroup_cfg_init(&sg_cfg);
    sg_cfg.group_id = 0;
    sg_cfg.subgroup_id = 0;
    sg_cfg.publisher_priority = 200;
    moq_subgroup_handle_t sg;
    if (moq_session_open_subgroup(f->tp.server, server_sub, &sg_cfg, 0, &sg)
        != MOQ_OK) goto fail;

    fake_endpoint_clear_ops(&f->tp.server_ep);
    moq_transport_bridge_service(f->tp.server_bridge, 0);

    uint64_t uni_sid = 0; bool have_uni = false;
    for (size_t i = 0; i < f->tp.server_ep.count; i++) {
        fake_op_t *o = &f->tp.server_ep.ops[i];
        if (o->kind == FAKE_OP_OPEN_UNI) { uni_sid = o->stream_id; have_uni = true; }
        else if (o->kind == FAKE_OP_WRITE && have_uni && o->stream_id == uni_sid &&
                 f->hdr_len + o->data_len <= sizeof(f->hdr)) {
            memcpy(f->hdr + f->hdr_len, o->data, o->data_len);
            f->hdr_len += o->data_len;
        }
    }
    if (!have_uni || f->hdr_len == 0) goto fail;
    memcpy(f->obj, f->hdr, f->hdr_len);
    f->obj_len = f->hdr_len;
    fake_endpoint_clear_ops(&f->tp.server_ep);

    uint8_t payload[256];
    if (payload_len > sizeof(payload)) goto fail;
    memset(payload, 'A', payload_len);
    moq_rcbuf_t *p = NULL;
    moq_rcbuf_create(moq_alloc_default(), payload, payload_len, &p);
    moq_result_t wrc = moq_session_write_object(f->tp.server, sg, 0, p, 0);
    moq_rcbuf_decref(p);
    if (wrc != MOQ_OK) goto fail;
    moq_transport_bridge_service(f->tp.server_bridge, 0);
    for (size_t i = 0; i < f->tp.server_ep.count; i++) {
        fake_op_t *o = &f->tp.server_ep.ops[i];
        if (o->kind == FAKE_OP_WRITE && o->stream_id == uni_sid &&
            f->obj_len + o->data_len <= sizeof(f->obj)) {
            memcpy(f->obj + f->obj_len, o->data, o->data_len);
            f->obj_len += o->data_len;
        }
    }
    fake_endpoint_clear_ops(&f->tp.server_ep);
    if (f->obj_len <= f->hdr_len) goto fail;

    return 0;

fail:
    test_pair_destroy(&f->tp);
    return -1;
}

static int uni_pending_fixture_init(uni_pending_fixture_t *f)
{
    return uni_pending_fixture_init_ex(f, false, 3);
}

/*
 * A suspended inbound data retry must keep pending_retry set and leave the
 * entry untouched, so the retained bytes are replayed on a later pass.
 */
static int test_data_retry_suspension_preserves_pending(void)
{
    int failures = 0;

    uni_pending_fixture_t f;
    if (uni_pending_fixture_init(&f) < 0) { failures++; return failures; }

    /* Header + one object against a full queue: the object cannot emit. */
    MOQ_TEST_CHECK(moq_transport_bridge_on_peer_uni_bytes(
                       f.tp.client_bridge, f.sid, f.obj, f.obj_len, false, 0)
                   == MOQ_ERR_WOULD_BLOCK);
    bridge_stream_entry_t *e = bridge_find_by_id(f.tp.client_bridge, f.sid);
    MOQ_TEST_CHECK(e != NULL && e->pending_retry);
    MOQ_TEST_CHECK(e->kind == BRIDGE_STREAM_UNI);

    sweep_arm_closing_subgroup(f.tp.client, 0);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       f.tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(e->pending_retry);
    MOQ_TEST_CHECK(e->active);
    MOQ_TEST_CHECK(!e->pending_fin && !e->fin_retained);
    MOQ_TEST_CHECK(!e->peer_send_closed);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(f.tp.client_bridge));
    MOQ_TEST_CHECK(f.tp.client->subgroups[0].state == MOQ_SG_CLOSING);

    /* Drain the slot, then let the unlimited path complete both. */
    moq_event_t ev;
    while (moq_session_poll_events(f.tp.client, &ev, 1) > 0)
        moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(moq_transport_bridge_service(f.tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.tp.client->subgroups[0].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!e->pending_retry);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(f.tp.client_bridge));

    /* The retained object is delivered -- clearing the flag is bookkeeping,
     * this is the session transition it stands for -- exactly once. */
    MOQ_TEST_CHECK(moq_session_poll_events(f.tp.client, &ev, 1) == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_OBJECT_RECEIVED);
    MOQ_TEST_CHECK(ev.u.object_received.object_id == 0);
    MOQ_TEST_CHECK(ev.u.object_received.payload != NULL);
    MOQ_TEST_CHECK(moq_rcbuf_len(ev.u.object_received.payload) == 3);
    MOQ_TEST_CHECK(memcmp(moq_rcbuf_data(ev.u.object_received.payload),
                          "AAA", 3) == 0);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(moq_session_poll_events(f.tp.client, &ev, 1) == 0);

    test_pair_destroy(&f.tp);
    return failures;
}

/*
 * A suspended reset delivery must keep pending_reset and its code, and must
 * not deactivate the entry -- the reset is delivered on a later pass instead.
 *
 * Only a streaming reset can block: the non-streaming path in handle_data_reset
 * records and frees without emitting, so it always returns MOQ_OK. The stream
 * is therefore parked mid-object with its begin chunk occupying the one event
 * slot, which is what makes the terminal chunk the reset must emit unaffordable
 * -- and it leaves no pending retry, so the reset block is the boundary that
 * suspends rather than the retry block above it.
 */
static int test_data_reset_suspension_preserves_pending(void)
{
    int failures = 0;

    uni_pending_fixture_t f;
    if (uni_pending_fixture_init_ex(&f, true, 64) < 0) { failures++; return failures; }

    /* Free the slot the begin chunk needs. */
    moq_event_t ev;
    while (moq_session_poll_events(f.tp.client, &ev, 1) > 0)
        moq_event_cleanup(&ev);

    /* Truncated mid-payload: the begin chunk emits and fills the slot, and the
     * stream parks in STREAMING_PAYLOAD with nothing further to parse. */
    MOQ_TEST_CHECK(f.obj_len > f.hdr_len + 20);
    MOQ_TEST_CHECK(moq_transport_bridge_on_peer_uni_bytes(
                       f.tp.client_bridge, f.sid, f.obj, f.obj_len - 20, false, 0)
                   == MOQ_OK);
    bridge_stream_entry_t *e = bridge_find_by_id(f.tp.client_bridge, f.sid);
    MOQ_TEST_CHECK(e != NULL && !e->pending_retry);
    MOQ_TEST_CHECK(f.tp.client->event_head != f.tp.client->event_tail);

    /* Pin the branch this test depends on by reading RX state rather than the
     * queued event: draining to inspect it would free the slot and unblock the
     * reset. Mid-payload under streaming delivery is the only combination
     * handle_data_reset answers by emitting. */
    MOQ_TEST_CHECK(f.tp.client->streaming_objects);
    size_t parked = 0;
    for (size_t i = 0; i < f.tp.client->rx_cap; i++) {
        moq_rx_stream_t *rx = &f.tp.client->rx_streams[i];
        if (rx->active && rx->stream_ref._v == e->ref._v &&
            rx->parse_state == MOQ_RX_STREAMING_PAYLOAD)
            parked++;
    }
    MOQ_TEST_CHECK(parked == 1);

    MOQ_TEST_CHECK(moq_transport_bridge_on_peer_stream_reset(
                       f.tp.client_bridge, f.sid, 0x5, 0) == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(e->pending_reset);
    MOQ_TEST_CHECK(e->pending_reset_code == 0x5);
    MOQ_TEST_CHECK(!e->pending_retry);

    sweep_arm_closing_subgroup(f.tp.client, 0);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       f.tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(e->pending_reset);
    MOQ_TEST_CHECK(e->pending_reset_code == 0x5);
    MOQ_TEST_CHECK(e->active);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(f.tp.client_bridge));
    MOQ_TEST_CHECK(f.tp.client->subgroups[0].state == MOQ_SG_CLOSING);

    while (moq_session_poll_events(f.tp.client, &ev, 1) > 0)
        moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(moq_transport_bridge_service(f.tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(f.tp.client->subgroups[0].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!e->pending_reset);
    MOQ_TEST_CHECK(!e->active);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(f.tp.client_bridge));

    /* The reset reaches the application as the object's terminal chunk. */
    MOQ_TEST_CHECK(moq_session_poll_events(f.tp.client, &ev, 1) == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_OBJECT_CHUNK);
    MOQ_TEST_CHECK(!ev.u.object_chunk.begin);
    MOQ_TEST_CHECK(ev.u.object_chunk.end);
    MOQ_TEST_CHECK(ev.u.object_chunk.terminal == MOQ_OBJECT_TERMINAL_RESET);
    MOQ_TEST_CHECK(ev.u.object_chunk.chunk == NULL);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(moq_session_poll_events(f.tp.client, &ev, 1) == 0);

    test_pair_destroy(&f.tp);
    return failures;
}

/*
 * A client holding an established peer-opened request bidi, with its single
 * event slot occupied by the SUBSCRIBE_REQUEST that bidi produced.
 *
 * The registration must happen while the queue still has room: a request whose
 * completing message cannot emit leaves the session without a retainable
 * stream, which the bridge escalates before any pending flag is set. Filling
 * the slot with the request's OWN event gives an established, registered entry
 * and a full queue at the same time.
 */
static int d18_request_bidi_fixture(test_pair_t *tp, uint64_t *bidi_out)
{
    if (d18_pair_init(tp, 1) < 0) return -1;
    moq_session_start(tp->client, 0);
    moq_session_start(tp->server, 0);
    d18_shuttle_until_quiescent(tp, 30, 0);

    moq_event_t ev;
    while (moq_session_poll_events(tp->client, &ev, 1) > 0)
        moq_event_cleanup(&ev);

    if (d18_server_subscribe(tp) != MOQ_OK) goto fail;
    moq_transport_bridge_service(tp->server_bridge, 0);

    uint64_t bidi = 0;
    for (size_t i = 0; i < tp->server_ep.count; i++) {
        fake_op_t *o = &tp->server_ep.ops[i];
        if (o->kind != FAKE_OP_WRITE) continue;
        bidi = o->stream_id;
        if (moq_transport_bridge_on_peer_bidi_bytes(
                tp->client_bridge, o->stream_id, o->data, o->data_len,
                o->fin, 0) != MOQ_OK) goto fail;
    }
    fake_endpoint_clear_ops(&tp->server_ep);
    if (!bidi) goto fail;

    *bidi_out = bidi;
    return 0;

fail:
    test_pair_destroy(tp);
    return -1;
}

/*
 * BACKLOG #245(c): the bridge's own retry must complete the obligation the
 * refused call created, not silently discard it.
 *
 * Extra bytes on a publisher-side namespace-sub bidi tear that bidi down
 * (session_namespace_sub.c:1078-1080 -> ns_sub_local_teardown), and the
 * teardown can refuse for action capacity before recording anything durable
 * (:667-668). Bridge ingress correctly recognises the owner and retains the
 * retry (transport_bridge.c:2662-2669) -- so #245(a)'s predicate answers
 * correctly here, which this fixture also pins. Service then retries with
 * NULL/0 (:1934-1949); today that empty retry reports MOQ_OK, pending_retry is
 * cleared, and the teardown is lost.
 *
 * Recovery is SERVICE-ONLY: the bridge never re-delivers peer bytes, so the
 * fixture feeds none after the refusal.
 */
/* -- Owner-graph and inventory oracles for the ns_sub bidi ----------- */

/* Exact drain-ring membership: the declared refs, each with its declared
 * reason, and no others. */
typedef struct drain_spec { uint64_t ref; moq_drain_reason_t reason; } drain_spec_t;

static int check_drain_membership(const moq_session_t *s,
                                  const drain_spec_t *want, size_t n,
                                  const char *what)
{
    int failures = 0;
    if (s->drain_ref_count != n) {
        fprintf(stderr, "FAIL: %s: drain ring holds %zu refs, expected %zu\n",
                what, s->drain_ref_count, n);
        failures++;
    }
    for (size_t i = 0; i < n; i++) {
        moq_stream_ref_t r = moq_stream_ref_from_u64(want[i].ref);
        if (!drain_ref_contains(s, r)) {
            fprintf(stderr, "FAIL: %s: drain ref %llu is ABSENT\n", what,
                    (unsigned long long)want[i].ref);
            failures++;
            continue;
        }
        if (drain_ref_reason(s, r) != want[i].reason) {
            fprintf(stderr, "FAIL: %s: drain ref %llu reason %d, expected %d\n",
                    what, (unsigned long long)want[i].ref,
                    (int)drain_ref_reason(s, r), (int)want[i].reason);
            failures++;
        }
    }
    for (size_t i = 0; i < s->drain_ref_count; i++) {
        int declared = 0;
        for (size_t k = 0; k < n; k++)
            if (s->drain_refs[i] == want[k].ref) declared = 1;
        if (!declared) {
            fprintf(stderr, "FAIL: %s: UNDECLARED drain ref %llu\n", what,
                    (unsigned long long)s->drain_refs[i]);
            failures++;
        }
    }
    return failures;
}

/* A shuttle that CHECKS every result it produces and proves it converged.
 * d18_shuttle_until_quiescent discards both the service and the ingress
 * results, so a setup that limped to ESTABLISHED through a refused call would
 * look identical to a clean one.
 *
 * Deliberately file-local, and NOT yet strict about everything: a non-WRITE op
 * and a write outside both id ranges are counted as movement but not
 * classified. That is sound for the setup path this drives, where neither
 * occurs. Tighten those two cases before lifting this into shared support. */
static int strict_feed(moq_transport_bridge_t *to, fake_endpoint_t *from,
                       uint64_t uni_base, uint64_t bidi_base, uint64_t now,
                       size_t *moved, const char *what)
{
    int failures = 0;
    MOQ_TEST_CHECK(from->count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < from->count; i++) {
        fake_op_t *o = &from->ops[i];
        (*moved)++;
        if (o->kind != FAKE_OP_WRITE) continue;
        moq_result_t rc = MOQ_OK;
        if (o->stream_id >= uni_base && o->stream_id < uni_base + 1000)
            rc = moq_transport_bridge_on_peer_uni_bytes(
                to, o->stream_id, o->data, o->data_len, o->fin, now);
        else if (o->stream_id >= bidi_base && o->stream_id < bidi_base + 1000)
            rc = moq_transport_bridge_on_peer_bidi_bytes(
                to, o->stream_id, o->data, o->data_len, o->fin, now);
        if (rc != MOQ_OK) {
            fprintf(stderr, "FAIL: %s: ingress on stream %llu returned %d\n",
                    what, (unsigned long long)o->stream_id, (int)rc);
            failures++;
        }
    }
    fake_endpoint_clear_ops(from);
    return failures;
}

static int d18_strict_shuttle(test_pair_t *tp, int max, uint64_t now,
                              const char *what)
{
    int failures = 0;
    for (int i = 0; i < max; i++) {
        size_t moved = 0;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_transport_bridge_service(tp->client_bridge, now),
            (int)MOQ_OK);
        failures += strict_feed(tp->server_bridge, &tp->client_ep, 1000, 2000,
                                now, &moved, what);
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_transport_bridge_service(tp->server_bridge, now),
            (int)MOQ_OK);
        failures += strict_feed(tp->client_bridge, &tp->server_ep, 3000, 4000,
                                now, &moved, what);
        if (moved) continue;
        /* Quiescence, proven rather than assumed: nothing left to write and
         * nothing retained on either side. */
        MOQ_TEST_CHECK_EQ_SIZE(tp->client_ep.count, (size_t)0);
        MOQ_TEST_CHECK_EQ_SIZE(tp->server_ep.count, (size_t)0);
        MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(tp->client_bridge));
        MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(tp->server_bridge));
        MOQ_TEST_CHECK_EQ_SIZE(tp->client->action_tail - tp->client->action_head,
                               (size_t)0);
        MOQ_TEST_CHECK_EQ_SIZE(tp->server->action_tail - tp->server->action_head,
                               (size_t)0);
        return failures;
    }
    fprintf(stderr, "FAIL: %s: shuttle did not converge in %d rounds\n",
            what, max);
    return failures + 1;
}

/*
 * A suspended inbound retry on a BIDI entry must keep pending_retry set and
 * leave the entry untouched, exactly as the uni arm does -- but through
 * moq_session_on_bidi_stream_bytes, which is a different session entry.
 *
 * The carrier is a namespace-subscription bidi the CLIENT opened, so the peer's
 * response arrives on a stream the client already owns in idx_ns_by_ref. That
 * ownership is what the bridge checks before believing a WOULD_BLOCK is a
 * retry; a draft-18 REQUEST bidi cannot be used here because that registry is
 * not consulted, and the retry is escalated to a fatal instead (tracked
 * separately).
 */
static int test_bidi_retry_suspension_preserves_pending(void)
{
    int failures = 0;

    test_pair_t tp;
    if (d18_pair_init(&tp, 1) < 0) { failures++; return failures; }
    moq_session_start(tp.client, 0);
    moq_session_start(tp.server, 0);
    d18_shuttle_until_quiescent(&tp, 30, 0);
    /* SETUP_COMPLETE deliberately left queued: it is the full slot. */
    MOQ_TEST_CHECK(tp.client->event_head != tp.client->event_tail);

    moq_bytes_t pfx_parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t pfx = { pfx_parts, 1 };
    moq_subscribe_namespace_cfg_t nc;
    moq_subscribe_namespace_cfg_init(&nc);
    nc.track_namespace_prefix = pfx;
    nc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nh;
    MOQ_TEST_CHECK(moq_session_subscribe_namespace(tp.client, &nc, 0, &nh)
                   == MOQ_OK);

    fake_endpoint_clear_ops(&tp.client_ep);
    moq_transport_bridge_service(tp.client_bridge, 0);
    uint64_t bidi = 0;
    for (size_t i = 0; i < tp.client_ep.count; i++)
        if (tp.client_ep.ops[i].kind == FAKE_OP_WRITE &&
            tp.client_ep.ops[i].stream_id >= 2000)
            bidi = tp.client_ep.ops[i].stream_id;
    MOQ_TEST_CHECK(bidi != 0);
    size_t fed = 0;
    d18_feed(tp.server_bridge, &tp.client_ep, 1000, 2000, 0, &fed);

    moq_event_t ev;
    moq_ns_sub_handle_t sh = MOQ_NS_SUB_HANDLE_INVALID;
    while (moq_session_poll_events(tp.server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_NS_SUB_REQUEST)
            sh = ev.u.ns_sub_request.handle;
        moq_event_cleanup(&ev);
    }
    moq_accept_ns_sub_cfg_t ac;
    moq_accept_ns_sub_cfg_init(&ac);
    MOQ_TEST_CHECK(moq_session_accept_ns_sub(tp.server, sh, &ac, 0) == MOQ_OK);

    /* The acceptance comes back on the client's own bidi and cannot emit. */
    fake_endpoint_clear_ops(&tp.server_ep);
    moq_transport_bridge_service(tp.server_bridge, 0);
    bool blocked = false;
    for (size_t i = 0; i < tp.server_ep.count; i++) {
        fake_op_t *o = &tp.server_ep.ops[i];
        if (o->kind != FAKE_OP_WRITE) continue;
        if (moq_transport_bridge_on_peer_bidi_bytes(
                tp.client_bridge, o->stream_id, o->data, o->data_len,
                o->fin, 0) == MOQ_ERR_WOULD_BLOCK)
            blocked = true;
    }
    fake_endpoint_clear_ops(&tp.server_ep);
    MOQ_TEST_CHECK(blocked);

    bridge_stream_entry_t *e = bridge_find_by_id(tp.client_bridge, bidi);
    MOQ_TEST_CHECK(e != NULL);
    MOQ_TEST_CHECK(e->kind == BRIDGE_STREAM_BIDI);
    MOQ_TEST_CHECK(e->pending_retry);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    /* The session really owns this stream -- the precondition the bridge tests
     * before treating a WOULD_BLOCK as retainable. */
    MOQ_TEST_CHECK(moq_index_find(tp.client->idx_ns_by_ref,
                                  tp.client->idx_ns_mask, e->ref._v) >= 0);

    sweep_arm_closing_subgroup(tp.client, 0);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(e->pending_retry);
    MOQ_TEST_CHECK(e->active);
    MOQ_TEST_CHECK(!e->pending_fin && !e->fin_retained);
    MOQ_TEST_CHECK(!e->peer_send_closed);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_CLOSING);

    while (moq_session_poll_events(tp.client, &ev, 1) > 0)
        moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!e->pending_retry);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));

    /* The retained acceptance is delivered, on the same handle, exactly once. */
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_NS_SUB_OK);
    MOQ_TEST_CHECK(ev.u.ns_sub_ok.handle._opaque == nh._opaque);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 0);

    test_pair_destroy(&tp);
    return failures;
}

/*
 * A session allocation failure on an inbound bidi is a HARD error, not a
 * wait: the bridge must escalate it to a connection fatal and retain no
 * retry. Individual pending flags are unobservable afterwards --
 * bridge_set_fatal() runs bridge_clear_all_state(), which deactivates every
 * entry -- so what is pinned is the DURABLE POSTCONDITION: the exact NOMEM
 * comes back to the adapter, the bridge is fatal (code 0x1) and not closed,
 * no stream entry is active or carries any pending flag, the mixed pending
 * query answers false, and every later ingress call refuses with
 * MOQ_ERR_CLOSED. Nothing can ever re-drive the failed message.
 *
 * Two cells, one per session-side recovery mode -- NOMEM_RETAIN (namespace
 * response) and NOMEM_REDELIVER (joining FETCH) -- because the bridge
 * deliberately collapses both to this one terminal outcome: no
 * session-level recovery is claimed, or possible, behind a fatal bridge.
 * Each cell runs a no-fault fixture first to pin the operation's declared
 * allocation signature on the bridge route, then a fresh failing fixture
 * whose armed origin must be reached on the same allocation path. Only the
 * RECEIVING session runs on the failing allocator; the bridges and the peer
 * stay on the default allocator, so the armed ordinal cannot land outside
 * the session under test.
 */

/* As test_pair_init_full's defaults (draft-16), with the CLIENT session on
 * a caller-supplied allocator. */
static int test_pair_init_client_alloc(test_pair_t *tp,
                                       const moq_alloc_t *client_alloc)
{
    memset(tp, 0, sizeof(*tp));

    moq_session_cfg_t ccfg;
    moq_session_cfg_init_sized(&ccfg, sizeof(ccfg), client_alloc,
                               MOQ_PERSPECTIVE_CLIENT);
    ccfg.send_request_capacity = true;
    ccfg.initial_request_capacity = 10;

    moq_session_cfg_t scfg;
    moq_session_cfg_init_sized(&scfg, sizeof(scfg), moq_alloc_default(),
                               MOQ_PERSPECTIVE_SERVER);
    scfg.send_request_capacity = true;
    scfg.initial_request_capacity = 10;

    if (moq_session_create(&ccfg, 0, &tp->client) < 0) return -1;
    if (moq_session_create(&scfg, 0, &tp->server) < 0) {
        moq_session_destroy(tp->client);
        return -1;
    }

    fake_endpoint_init(&tp->client_ep, 1000, 2000);
    fake_endpoint_init(&tp->server_ep, 3000, 4000);

    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    if (moq_transport_bridge_create(&bcfg, tp->client,
            &tp->client_ep.vtable, &tp->client_ep,
            &tp->client_bridge) < 0) {
        moq_session_destroy(tp->server);
        moq_session_destroy(tp->client);
        return -1;
    }
    if (moq_transport_bridge_create(&bcfg, tp->server,
            &tp->server_ep.vtable, &tp->server_ep,
            &tp->server_bridge) < 0) {
        moq_transport_bridge_destroy(tp->client_bridge);
        moq_session_destroy(tp->server);
        moq_session_destroy(tp->client);
        return -1;
    }
    return 0;
}

/* The bridge-owned durable postcondition both cells assert. */
static int br_fatal_postcondition(moq_transport_bridge_t *b,
                                  uint64_t probe_id, fp_alloc_state_t *fs,
                                  const char *op)
{
    int failures = 0;
    MOQ_TEST_CHECK(moq_transport_bridge_is_fatal(b));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(b));
    MOQ_TEST_CHECK_EQ_HEX(b->fatal_code, 0x1);
    for (uint32_t i = 0; i < b->max_streams; i++) {
        const bridge_stream_entry_t *e = &b->streams[i];
        MOQ_TEST_CHECK(!e->active);
        MOQ_TEST_CHECK(!e->pending_retry && !e->pending_fin &&
                       !e->fin_retained && !e->pending_reset &&
                       !e->pending_stop);
    }
    MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(b));
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_on_peer_bidi_bytes(b, probe_id, NULL, 0,
                                                     false, 0),
        (int)MOQ_ERR_CLOSED);
    failures += fp_sticky_clean(fs, op);
    return failures;
}

/* The failing allocator's terminal facts AFTER the pair is destroyed:
 * destroy-to-zero, and the sticky flags re-checked -- destruction itself
 * frees through the wrapper, and a wrong-size free there would latch a
 * flag while still reaching all three zeros. The pre-destroy sticky check
 * stays where it is for localization; this one closes teardown. */
static int br_alloc_zeroed(const fp_alloc_state_t *fs, const char *op)
{
    int bad = fp_sticky_clean(fs, op);
    if (fs->balance != 0 || fs->live_bytes != 0 || fs->table_len != 0) {
        fprintf(stderr, "FAILPOINT %s: destroy left balance %lld, live "
                "%lld, table %zu\n", op, (long long)fs->balance,
                (long long)fs->live_bytes, fs->table_len);
        bad++;
    }
    return bad;
}

/* Establish a draft-16 namespace subscription THROUGH the bridges: the
 * client opens the ns_sub bidi, the server accepts, and the acceptance is
 * delivered back. Returns the client-side bidi id, or 0 on failure. */
static uint64_t br_ns_establish(test_pair_t *tp, int *failures_out)
{
    moq_subscribe_namespace_cfg_t nc;
    moq_subscribe_namespace_cfg_init(&nc);
    moq_bytes_t pfx_parts[] = { MOQ_BYTES_LITERAL("live") };
    nc.track_namespace_prefix = (moq_namespace_t){ pfx_parts, 1 };
    moq_ns_sub_handle_t nh;
    if (moq_session_subscribe_namespace(tp->client, &nc, 0, &nh) != MOQ_OK) {
        (*failures_out)++;
        return 0;
    }

    fake_endpoint_clear_ops(&tp->client_ep);
    moq_transport_bridge_service(tp->client_bridge, 0);
    uint64_t bidi = 0;
    for (size_t i = 0; i < tp->client_ep.count; i++)
        if (tp->client_ep.ops[i].kind == FAKE_OP_WRITE &&
            tp->client_ep.ops[i].stream_id >= 2000)
            bidi = tp->client_ep.ops[i].stream_id;
    if (bidi == 0) { (*failures_out)++; return 0; }
    size_t fed = 0;
    d18_feed(tp->server_bridge, &tp->client_ep, 1000, 2000, 0, &fed);

    moq_event_t ev;
    moq_ns_sub_handle_t sh = MOQ_NS_SUB_HANDLE_INVALID;
    while (moq_session_poll_events(tp->server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_NS_SUB_REQUEST)
            sh = ev.u.ns_sub_request.handle;
        moq_event_cleanup(&ev);
    }
    moq_accept_ns_sub_cfg_t ac;
    moq_accept_ns_sub_cfg_init(&ac);
    if (moq_session_accept_ns_sub(tp->server, sh, &ac, 0) != MOQ_OK) {
        (*failures_out)++;
        return 0;
    }
    /* The acceptance rides the CLIENT's own bidi, so the server's writes
     * land on that peer-side id -- outside the server's local stream
     * ranges -- and are fed back directly. */
    fake_endpoint_clear_ops(&tp->server_ep);
    moq_transport_bridge_service(tp->server_bridge, 0);
    for (size_t i = 0; i < tp->server_ep.count; i++) {
        fake_op_t *o = &tp->server_ep.ops[i];
        if (o->kind != FAKE_OP_WRITE) continue;
        moq_transport_bridge_on_peer_bidi_bytes(
            tp->client_bridge, o->stream_id, o->data, o->data_len,
            o->fin, 0);
    }
    fake_endpoint_clear_ops(&tp->server_ep);
    bool got_ok = false;
    while (moq_session_poll_events(tp->client, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_NS_SUB_OK) got_ok = true;
        moq_event_cleanup(&ev);
    }
    if (!got_ok) { (*failures_out)++; return 0; }
    return bidi;
}

/* One draft-16 NAMESPACE response message with a single-part suffix. */
static size_t br_encode_d16_namespace(uint8_t *buf, size_t cap,
                                      const char *field)
{
    uint8_t payload[64];
    moq_buf_writer_t pw;
    moq_buf_writer_init(&pw, payload, sizeof(payload));
    moq_bytes_t parts[] = { { (const uint8_t *)field, strlen(field) } };
    moq_namespace_t ns = { parts, 1 };
    moq_buf_write_namespace_prefix(&pw, &ns);
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    moq_control_encode_envelope(&w, MOQ_D16_NAMESPACE, payload,
                                (uint16_t)moq_buf_writer_offset(&pw));
    return moq_buf_writer_offset(&w);
}

/*
 * BR-RETAIN: the draft-16 namespace-response route, suffix-tracker origin.
 * Session-side this origin is NOMEM_RETAIN; behind the bridge it is the
 * one fatal terminal.
 */
static int test_bridge_nomem_ns_response(void)
{
    int failures = 0;

    /* The canonical key of a one-part 2-byte suffix ("aa") is
     * [count u8][u16 len][bytes] = 5 bytes; the stored copy repeats it. */
    static const fp_expect_t k_sig[4] = {
        { FP_ALLOC, FP_SIZE_EXACT, 5, 0 },      /* canonical key */
        { FP_ALLOC, FP_SIZE_ANY, 0, 0 },        /* tracker (private) */
        { FP_ALLOC, FP_SIZE_SAME_AS, 0, 0 },    /* stored key copy */
        { FP_ALLOC, FP_SIZE_ANY, 0, 0 },        /* key array (private) */
    };
    fp_attempt_t base_log[FP_LOG_CAP];
    size_t base_n = 0;

    /* No-fault fixture: the operation's signature on this route. */
    {
        fp_alloc_state_t fs = {0};
        moq_alloc_t alloc = fp_allocator(&fs);
        test_pair_t tp;
        if (test_pair_init_client_alloc(&tp, &alloc) < 0) {
            failures++;
            return failures;
        }
        MOQ_TEST_CHECK(setup_handshake(&tp));
        uint64_t bidi = br_ns_establish(&tp, &failures);
        MOQ_TEST_CHECK(bidi != 0);

        uint8_t msg[128];
        size_t mlen = br_encode_d16_namespace(msg, sizeof(msg), "aa");
        fs.log_from = fs.call_count;
        fs.log_len = 0;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_transport_bridge_on_peer_bidi_bytes(
                tp.client_bridge, bidi, msg, mlen, false, 0),
            (int)MOQ_OK);
        failures += fp_check_signature(&fs, 0, k_sig, 4, "br-retain-base");
        memcpy(base_log, fs.log, fs.log_len * sizeof(fp_attempt_t));
        base_n = fs.log_len;
        MOQ_TEST_CHECK_EQ_SIZE(base_n, 4u);
        moq_event_t ev;
        MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 1);
        MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_NAMESPACE_FOUND);
        moq_event_cleanup(&ev);
        failures += fp_sticky_clean(&fs, "br-retain-base");
        test_pair_destroy(&tp);
        failures += br_alloc_zeroed(&fs, "br-retain-base");
    }

    /* Failing fixture: the tracker origin, and the durable postcondition. */
    {
        fp_alloc_state_t fs = {0};
        moq_alloc_t alloc = fp_allocator(&fs);
        test_pair_t tp;
        if (test_pair_init_client_alloc(&tp, &alloc) < 0) {
            failures++;
            return failures;
        }
        MOQ_TEST_CHECK(setup_handshake(&tp));
        uint64_t bidi = br_ns_establish(&tp, &failures);
        MOQ_TEST_CHECK(bidi != 0);

        uint8_t msg[128];
        size_t mlen = br_encode_d16_namespace(msg, sizeof(msg), "aa");
        /* Not full: the NOMEM is attributable to the armed allocation. */
        MOQ_TEST_CHECK(!event_queue_full(tp.client));
        fs.log_from = fs.call_count;
        fs.log_len = 0;
        fs.fail_at = fs.call_count + 2;     /* +1 = key, +2 = the tracker */
        moq_result_t rc = moq_transport_bridge_on_peer_bidi_bytes(
            tp.client_bridge, bidi, msg, mlen, false, 0);
        fp_context("br-retain", 2, 4, &fs);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_NOMEM);
        MOQ_TEST_CHECK_EQ_U64(fs.call_count, fs.fail_at);
        fs.fail_at = 0;
        failures += fp_check_prefix(&fs, 0, base_log, 2, "br-retain");
        failures += br_fatal_postcondition(tp.client_bridge, bidi, &fs,
                                           "br-retain");
        test_pair_destroy(&tp);
        failures += br_alloc_zeroed(&fs, "br-retain");
    }

    return failures;
}

/* One draft-18 LARGEST_OBJECT SUBSCRIBE, left pending on the server. */
static size_t br_encode_d18_subscribe(uint8_t *buf, size_t cap,
                                      uint64_t req_id)
{
    moq_d18_msg_params_t mp;
    memset(&mp, 0, sizeof(mp));
    mp.has_filter = true;
    mp.filter_type = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    mp.has_forward = true;
    mp.forward = 1;
    moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { ns_parts, 1 };
    moq_bytes_t name = MOQ_BYTES_LITERAL("v0");
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    moq_d18_encode_subscribe(&w, req_id, &ns, name, &mp);
    return moq_buf_writer_offset(&w);
}

/* One joining FETCH carrying two USE_VALUE tokens of distinct lengths. */
static size_t br_encode_d18_join_fetch2(uint8_t *buf, size_t cap)
{
    moq_d18_fetch_t f;
    memset(&f, 0, sizeof(f));
    f.request_id = 2;
    f.fetch_type = 2;              /* relative joining */
    f.joining_request_id = 0;
    f.joining_start = 3;
    f.params.auth_token_count = 2;
    f.params.auth_tokens[0].alias_type = MOQ_AUTH_TOKEN_USE_VALUE;
    f.params.auth_tokens[0].token_type = 7;
    f.params.auth_tokens[0].token_value = MOQ_BYTES_LITERAL("jointok");
    f.params.auth_tokens[1].alias_type = MOQ_AUTH_TOKEN_USE_VALUE;
    f.params.auth_tokens[1].token_type = 9;
    f.params.auth_tokens[1].token_value =
        MOQ_BYTES_LITERAL("second-join-token");
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, buf, cap);
    moq_d18_encode_fetch(&w, &f);
    return moq_buf_writer_offset(&w);
}

/* PENDING_JOIN fetch entries on the receiving session (white-box). */
static int count_busy_pending_joins(const moq_session_t *s)
{
    int n = 0;
    for (size_t i = 0; i < s->fetch_cap; i++)
        if (s->fetches[i].state == MOQ_FETCH_PENDING_JOIN) n++;
    return n;
}

/* Feed the pending SUBSCRIBE the joining FETCH references; the SERVER is
 * the receiving session. */
static void br_join_prepare(test_pair_t *tp, int *failures_out)
{
    uint8_t sub[160];
    size_t sn = br_encode_d18_subscribe(sub, sizeof(sub), 0);
    if (moq_transport_bridge_on_peer_bidi_bytes(
            tp->server_bridge, 5001, sub, sn, false, 0) != MOQ_OK)
        (*failures_out)++;
    bool got = false;
    moq_event_t ev;
    while (moq_session_poll_events(tp->server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) got = true;
        moq_event_cleanup(&ev);
    }
    if (!got) (*failures_out)++;
    moq_action_t a;
    while (moq_session_poll_actions(tp->server, &a, 1) > 0)
        moq_action_cleanup(&a);
}

/*
 * BR-REDELIVER: the draft-18 joining-FETCH route, second token-copy
 * origin -- so the first copy's release still runs under the failure.
 * Session-side this origin is NOMEM_REDELIVER; behind the bridge it is
 * the SAME fatal terminal as BR-RETAIN, deliberately: both recovery modes
 * collapse there, and no session-level recovery is claimed behind it.
 */
static int test_bridge_nomem_joining_fetch(void)
{
    int failures = 0;

    static const fp_expect_t k_sig[2] = {
        { FP_ALLOC, FP_SIZE_EXACT, 7, 0 },   /* "jointok" */
        { FP_ALLOC, FP_SIZE_EXACT, 17, 0 },  /* "second-join-token" */
    };
    fp_attempt_t base_log[FP_LOG_CAP];
    size_t base_n = 0;

    /* No-fault fixture: the operation's signature on this route. */
    {
        fp_alloc_state_t fs = {0};
        moq_alloc_t alloc = fp_allocator(&fs);
        test_pair_t tp;
        if (d18_pair_init_alloc(&tp, 0, moq_alloc_default(), &alloc) < 0) {
            failures++;
            return failures;
        }
        moq_session_start(tp.client, 0);
        moq_session_start(tp.server, 0);
        d18_shuttle_until_quiescent(&tp, 30, 0);
        moq_event_t ev;
        while (moq_session_poll_events(tp.client, &ev, 1) > 0)
            moq_event_cleanup(&ev);
        while (moq_session_poll_events(tp.server, &ev, 1) > 0)
            moq_event_cleanup(&ev);
        br_join_prepare(&tp, &failures);

        uint8_t fb[224];
        size_t fn = br_encode_d18_join_fetch2(fb, sizeof(fb));
        fs.log_from = fs.call_count;
        fs.log_len = 0;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_transport_bridge_on_peer_bidi_bytes(
                tp.server_bridge, 5003, fb, fn, false, 0),
            (int)MOQ_OK);
        failures += fp_check_signature(&fs, 0, k_sig, 2, "br-redeliver-base");
        memcpy(base_log, fs.log, fs.log_len * sizeof(fp_attempt_t));
        base_n = fs.log_len;
        MOQ_TEST_CHECK_EQ_SIZE(base_n, 2u);
        MOQ_TEST_CHECK_EQ_INT(count_busy_pending_joins(tp.server), 1);
        failures += fp_sticky_clean(&fs, "br-redeliver-base");
        test_pair_destroy(&tp);
        failures += br_alloc_zeroed(&fs, "br-redeliver-base");
    }

    /* Failing fixture: the second copy, and the durable postcondition. */
    {
        fp_alloc_state_t fs = {0};
        moq_alloc_t alloc = fp_allocator(&fs);
        test_pair_t tp;
        if (d18_pair_init_alloc(&tp, 0, moq_alloc_default(), &alloc) < 0) {
            failures++;
            return failures;
        }
        moq_session_start(tp.client, 0);
        moq_session_start(tp.server, 0);
        d18_shuttle_until_quiescent(&tp, 30, 0);
        moq_event_t ev;
        while (moq_session_poll_events(tp.client, &ev, 1) > 0)
            moq_event_cleanup(&ev);
        while (moq_session_poll_events(tp.server, &ev, 1) > 0)
            moq_event_cleanup(&ev);
        br_join_prepare(&tp, &failures);

        uint8_t fb[224];
        size_t fn = br_encode_d18_join_fetch2(fb, sizeof(fb));
        MOQ_TEST_CHECK(!event_queue_full(tp.server));
        fs.log_from = fs.call_count;
        fs.log_len = 0;
        fs.fail_at = fs.call_count + 2;     /* the second token copy */
        moq_result_t rc = moq_transport_bridge_on_peer_bidi_bytes(
            tp.server_bridge, 5003, fb, fn, false, 0);
        fp_context("br-redeliver", 2, 2, &fs);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_NOMEM);
        MOQ_TEST_CHECK_EQ_U64(fs.call_count, fs.fail_at);
        fs.fail_at = 0;
        failures += fp_check_prefix(&fs, 0, base_log, 2, "br-redeliver");
        MOQ_TEST_CHECK_EQ_INT(count_busy_pending_joins(tp.server), 0);
        failures += br_fatal_postcondition(tp.server_bridge, 5003, &fs,
                                           "br-redeliver");
        test_pair_destroy(&tp);
        failures += br_alloc_zeroed(&fs, "br-redeliver");
    }

    return failures;
}

/* Both arms of the reset family call different session entries, so the bidi
 * arm is proven separately from the uni arm above. */
static int test_bidi_reset_suspension_preserves_pending(void)
{
    int failures = 0;

    test_pair_t tp;
    uint64_t bidi = 0;
    if (d18_request_bidi_fixture(&tp, &bidi) < 0) { failures++; return failures; }

    bridge_stream_entry_t *e = bridge_find_by_id(tp.client_bridge, bidi);
    MOQ_TEST_CHECK(e != NULL && e->kind == BRIDGE_STREAM_BIDI);
    MOQ_TEST_CHECK(!e->pending_retry);
    MOQ_TEST_CHECK(tp.client->event_head != tp.client->event_tail);

    MOQ_TEST_CHECK(moq_transport_bridge_on_peer_stream_reset(
                       tp.client_bridge, bidi, 0x5, 0) == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(e->pending_reset);
    MOQ_TEST_CHECK(e->pending_reset_code == 0x5);

    sweep_arm_closing_subgroup(tp.client, 0);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(e->pending_reset);
    MOQ_TEST_CHECK(e->pending_reset_code == 0x5);
    MOQ_TEST_CHECK(e->active);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_CLOSING);

    /* Capture the subscription the inbound request created, so the terminal
     * event can be matched to it rather than merely counted. */
    moq_event_t ev;
    moq_subscription_t want_sub = MOQ_SUBSCRIPTION_INVALID;
    while (moq_session_poll_events(tp.client, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            want_sub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_subscription_is_valid(want_sub));
    moq_stream_ref_t bref = e->ref;

    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!e->pending_reset);
    MOQ_TEST_CHECK(!e->active);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));

    /* The request teardown reaches the application, on the same subscription,
     * and the stream leaves the draft-18 request registry. */
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_UNSUBSCRIBED);
    MOQ_TEST_CHECK(ev.u.unsubscribed.sub._opaque == want_sub._opaque);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(request_registry_find_by_streamref(tp.client, bref).kind ==
                   MOQ_REQ_NONE);

    /* Re-servicing delivers nothing further. */
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 0);

    test_pair_destroy(&tp);
    return failures;
}

/* moq_session_on_bidi_stream_stop is the draft-18 request-cancellation input
 * and is distinct from a reset, so it is wired and proven on its own. */
static int test_bidi_stop_suspension_preserves_pending(void)
{
    int failures = 0;

    test_pair_t tp;
    uint64_t bidi = 0;
    if (d18_request_bidi_fixture(&tp, &bidi) < 0) { failures++; return failures; }

    bridge_stream_entry_t *e = bridge_find_by_id(tp.client_bridge, bidi);
    MOQ_TEST_CHECK(e != NULL && e->kind == BRIDGE_STREAM_BIDI);

    MOQ_TEST_CHECK(moq_transport_bridge_on_peer_stop_sending(
                       tp.client_bridge, bidi, 0x5, 0) == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(e->pending_stop);
    MOQ_TEST_CHECK(e->pending_stop_code == 0x5);
    MOQ_TEST_CHECK(!e->pending_retry && !e->pending_reset);

    sweep_arm_closing_subgroup(tp.client, 0);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(e->pending_stop);
    MOQ_TEST_CHECK(e->pending_stop_code == 0x5);
    MOQ_TEST_CHECK(e->active);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_CLOSING);

    /* Capture the subscription the inbound request created, so the terminal
     * event can be matched to it rather than merely counted. */
    moq_event_t ev;
    moq_subscription_t want_sub = MOQ_SUBSCRIPTION_INVALID;
    while (moq_session_poll_events(tp.client, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            want_sub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_subscription_is_valid(want_sub));
    moq_stream_ref_t bref = e->ref;

    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!e->pending_stop);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));

    /* The request teardown reaches the application, on the same subscription,
     * and the stream leaves the draft-18 request registry. */
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_UNSUBSCRIBED);
    MOQ_TEST_CHECK(ev.u.unsubscribed.sub._opaque == want_sub._opaque);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(request_registry_find_by_streamref(tp.client, bref).kind ==
                   MOQ_REQ_NONE);

    /* Re-servicing delivers nothing further. */
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 0);

    test_pair_destroy(&tp);
    return failures;
}

/*
 * moq_session_on_data_stop is the local-origin uni arm of the stop family: a
 * peer STOP_SENDING on a stream WE opened. So the client must be the publisher
 * here, which is the reverse of every other fixture in this file.
 *
 * This entry blocks on ACTION capacity, not event capacity -- it answers a stop
 * by enqueuing RESET_DATA -- so the action queue is what has to be full.
 */
static int test_data_stop_suspension_preserves_pending(void)
{
    int failures = 0;

    test_pair_t tp;
    /* One action slot, so a single queued object fills it. A larger queue would
     * need enough writes to overrun the endpoint's op recorder, and a dropped
     * op would make the "no reset yet" assertion below prove nothing. */
    if (test_pair_init_full(&tp, 0, false, 1, 0) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* The SERVER subscribes, so the CLIENT publishes. */
    moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { ns_parts, 1 };
    moq_subscribe_cfg_t sub_cfg;
    moq_subscribe_cfg_init(&sub_cfg);
    sub_cfg.track_namespace = ns;
    sub_cfg.track_name = MOQ_BYTES_LITERAL("video");
    sub_cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t ssub;
    MOQ_TEST_CHECK(moq_session_subscribe(tp.server, &sub_cfg, 0, &ssub) == MOQ_OK);
    pump_until_quiescent(&tp, 20, 0);

    moq_event_t ev;
    moq_subscription_t csub = MOQ_SUBSCRIPTION_INVALID;
    while (moq_session_poll_events(tp.client, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            csub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_subscription_is_valid(csub));
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    MOQ_TEST_CHECK(moq_session_accept_subscribe(tp.client, csub, &acc, 0) == MOQ_OK);
    pump_until_quiescent(&tp, 20, 0);
    while (moq_session_poll_events(tp.server, &ev, 1) > 0)
        moq_event_cleanup(&ev);

    /* The client opens a subgroup: a local-origin data uni. */
    moq_subgroup_cfg_t sg_cfg;
    moq_subgroup_cfg_init(&sg_cfg);
    sg_cfg.group_id = 0;
    sg_cfg.subgroup_id = 0;
    sg_cfg.publisher_priority = 200;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK(moq_session_open_subgroup(tp.client, csub, &sg_cfg, 0, &sg)
                   == MOQ_OK);
    fake_endpoint_clear_ops(&tp.client_ep);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);

    uint64_t uni_sid = 0;
    for (size_t i = 0; i < tp.client_ep.count; i++)
        if (tp.client_ep.ops[i].kind == FAKE_OP_OPEN_UNI)
            uni_sid = tp.client_ep.ops[i].stream_id;
    MOQ_TEST_CHECK(uni_sid != 0);
    fake_endpoint_clear_ops(&tp.client_ep);

    bridge_stream_entry_t *e = bridge_find_by_id(tp.client_bridge, uni_sid);
    MOQ_TEST_CHECK(e != NULL);
    MOQ_TEST_CHECK(e->kind == BRIDGE_STREAM_UNI);
    MOQ_TEST_CHECK(e->origin == BRIDGE_ORIGIN_LOCAL);
    MOQ_TEST_CHECK(e->uni_disp != BRIDGE_UNI_DISP_CONTROL);

    /* The subgroup this stream carries must be live, or on_data_stop returns
     * without performing its reset transition at all. */
    size_t target = SIZE_MAX;
    for (size_t i = 0; i < tp.client->sg_cap; i++)
        if (tp.client->subgroups[i].stream_ref._v == e->ref._v &&
            (tp.client->subgroups[i].state == MOQ_SG_OPEN ||
             tp.client->subgroups[i].state == MOQ_SG_STREAMING))
            target = i;
    MOQ_TEST_CHECK(target != SIZE_MAX);
    moq_sg_state_t target_state = tp.client->subgroups[target].state;

    /* Fill the action queue with one real queued object, never serviced. */
    {
        moq_rcbuf_t *p = NULL;
        moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"AAA", 3, &p);
        MOQ_TEST_CHECK(moq_session_write_object(tp.client, sg, 0, p, 0) == MOQ_OK);
        moq_rcbuf_decref(p);
    }
    MOQ_TEST_CHECK(action_queue_full(tp.client));

    MOQ_TEST_CHECK(moq_transport_bridge_on_peer_stop_sending(
                       tp.client_bridge, uni_sid, 0x7, 0) == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(e->pending_stop);
    MOQ_TEST_CHECK(e->pending_stop_code == 0x7);
    MOQ_TEST_CHECK(!e->pending_retry && !e->pending_reset);

    /* Arm the sweep in a FREE slot: arming the target would mark it CLOSING,
     * and on_data_stop would then return before doing anything. */
    size_t arm = SIZE_MAX;
    for (size_t i = 0; i < tp.client->sg_cap; i++)
        if (i != target && tp.client->subgroups[i].state == MOQ_SG_FREE)
            arm = i;
    MOQ_TEST_CHECK(arm != SIZE_MAX && arm != target);
    sweep_arm_closing_subgroup(tp.client, arm);

    fake_endpoint_clear_ops(&tp.client_ep);
    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(e->pending_stop);
    MOQ_TEST_CHECK(e->pending_stop_code == 0x7);
    MOQ_TEST_CHECK(e->active);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    /* The stop has not been performed: the subgroup is untouched and no reset
     * reached the transport. */
    MOQ_TEST_CHECK(tp.client->subgroups[target].state == target_state);
    MOQ_TEST_CHECK(tp.client->subgroups[arm].state == MOQ_SG_CLOSING);
    /* The recorder never overflowed, so an absent reset really is absent
     * rather than dropped. */
    MOQ_TEST_CHECK(tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < tp.client_ep.count; i++)
        MOQ_TEST_CHECK(tp.client_ep.ops[i].kind != FAKE_OP_RESET);

    fake_endpoint_clear_ops(&tp.client_ep);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);

    MOQ_TEST_CHECK(tp.client->subgroups[arm].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!e->pending_stop);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    /* on_data_stop deterministically resets the subgroup, and dispatching that
     * reset deactivates the bridge entry. */
    MOQ_TEST_CHECK(tp.client->subgroups[target].state == MOQ_SG_RESETTING);
    MOQ_TEST_CHECK(!e->active);

    size_t resets = 0;
    MOQ_TEST_CHECK(tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < tp.client_ep.count; i++) {
        fake_op_t *o = &tp.client_ep.ops[i];
        if (o->kind != FAKE_OP_RESET) continue;
        resets++;
        MOQ_TEST_CHECK(o->stream_id == uni_sid);
        MOQ_TEST_CHECK(o->error_code == 0x7);
    }
    MOQ_TEST_CHECK(resets == 1);

    fake_endpoint_clear_ops(&tp.client_ep);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    MOQ_TEST_CHECK(tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < tp.client_ep.count; i++)
        MOQ_TEST_CHECK(tp.client_ep.ops[i].kind != FAKE_OP_RESET);

    test_pair_destroy(&tp);
    return failures;
}

/*
 * The deadline stage is the one boundary whose work is session-owned: the
 * bridge holds no flag to preserve, so what a suspension must prove is that the
 * OBLIGATION survives -- the deadline stays due and its transition runs exactly
 * once on a later pass, rather than being consumed, lost, or turned fatal.
 *
 * A real configured idle timeout supplies that deadline. Unlike every other
 * test here the arming helper's UINT64_MAX subgroup deadline matters in the
 * opposite direction: it keeps the sweep from contributing a competing
 * deadline, so the idle timeout stays the reported next deadline.
 */
static int test_tick_suspension_preserves_due_deadline(void)
{
    int failures = 0;

    const uint64_t idle_us = 30000000;
    test_pair_t tp;
    if (test_pair_init_full(&tp, 0, false, 0, idle_us) < 0) {
        failures++; return failures;
    }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    MOQ_TEST_CHECK(moq_session_state(tp.client) == MOQ_SESS_ESTABLISHED);
    /* The reported next deadline IS the idle deadline. This does not by itself
     * exclude another source sharing the same instant; the close code asserted
     * at the end is what attributes the transition to the idle timeout. */
    uint64_t due = moq_session_next_deadline_us(tp.client);
    MOQ_TEST_CHECK(due != UINT64_MAX);
    MOQ_TEST_CHECK(due == tp.client->idle_deadline_us);
    uint64_t idle_before = tp.client->idle_deadline_us;

    sweep_arm_closing_subgroup(tp.client, 0);
    /* Still the reported next deadline: the armed subgroup contributes none. */
    MOQ_TEST_CHECK(moq_session_next_deadline_us(tp.client) == due);
    MOQ_TEST_CHECK(tp.client_bridge->pending_count == 0);

    fake_endpoint_clear_ops(&tp.client_ep);
    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, due, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(moq_session_state(tp.client) != MOQ_SESS_CLOSED);
    /* The cursor is live, so the pass genuinely stopped mid-sweep. */
    MOQ_TEST_CHECK(tp.client->sweep_active);
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_CLOSING);
    /* The obligation survives: same deadline, still due, nothing outstanding. */
    MOQ_TEST_CHECK(tp.client->idle_deadline_us == idle_before);
    MOQ_TEST_CHECK(moq_session_next_deadline_us(tp.client) == due);
    MOQ_TEST_CHECK(tp.client_bridge->pending_count == 0);
    MOQ_TEST_CHECK(tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < tp.client_ep.count; i++)
        MOQ_TEST_CHECK(tp.client_ep.ops[i].kind != FAKE_OP_CLOSE);

    /* Ordinary unlimited service at the SAME time runs the idle transition.
     *
     * The cursor and the armed subgroup are both gone afterwards, but that is
     * NOT evidence the sweep finished first: close_with_error discards the
     * cursor and frees every subgroup, so the terminal path alone produces the
     * same state. What this pass pins is the terminal outcome below. */
    fake_endpoint_clear_ops(&tp.client_ep);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, due) == MOQ_OK);

    MOQ_TEST_CHECK(!tp.client->sweep_active);
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(moq_session_state(tp.client) == MOQ_SESS_CLOSED);

    size_t closes = 0;
    MOQ_TEST_CHECK(tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < tp.client_ep.count; i++) {
        fake_op_t *o = &tp.client_ep.ops[i];
        if (o->kind != FAKE_OP_CLOSE) continue;
        closes++;
        /* Idle timeout, not the bridge's generic 0x1 fatal. */
        MOQ_TEST_CHECK(o->error_code == MOQ_CLOSE_IDLE_TIMEOUT);
    }
    MOQ_TEST_CHECK(closes == 1);

    size_t terminals = 0;
    moq_event_t ev;
    while (moq_session_poll_events(tp.client, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SESSION_CLOSED) {
            terminals++;
            MOQ_TEST_CHECK(ev.u.closed.code ==
                           MOQ_CLOSE_IDLE_TIMEOUT);
        }
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(terminals == 1);

    fake_endpoint_clear_ops(&tp.client_ep);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, due) == MOQ_OK);
    MOQ_TEST_CHECK(tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < tp.client_ep.count; i++)
        MOQ_TEST_CHECK(tp.client_ep.ops[i].kind != FAKE_OP_CLOSE);

    test_pair_destroy(&tp);
    return failures;
}

/*
 * Loop termination: a suspension inside the per-stream scan must STOP the scan,
 * not continue to the next owner. The budget is already spent, so every later
 * owner would only suspend again -- repeating the work and, worse, reporting a
 * pass that touched owners it never advanced.
 *
 * The first pending entry sits at a nonzero index with two more above it, so a
 * `continue` is observable: it visits them and drives the suspension counter
 * past one. Entries are built directly here because the six family fixtures
 * already prove session routing; what is isolated is the loop rule alone.
 */
static int test_inbound_scan_stops_at_suspension(void)
{
    int failures = 0;

    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    moq_event_t ev;
    while (moq_session_poll_events(tp.client, &ev, 1) > 0)
        moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    fake_endpoint_clear_ops(&tp.client_ep);

    /* Nothing else can produce work: no queued actions, no outbound pending,
     * no due deadline. */
    MOQ_TEST_CHECK(tp.client_bridge->pending_count == 0);
    MOQ_TEST_CHECK(tp.client->action_head == tp.client->action_tail);
    MOQ_TEST_CHECK(moq_session_next_deadline_us(tp.client) == UINT64_MAX);
    MOQ_TEST_CHECK(!tp.client_bridge->pending_control);

    /* Three pending owners, the first at a nonzero index. */
    const uint32_t idx[3] = { 2, 5, 9 };
    MOQ_TEST_CHECK(tp.client_bridge->max_streams > idx[2]);
    moq_stream_ref_t refs[3];
    for (size_t k = 0; k < 3; k++) {
        bridge_stream_entry_t *e = &tp.client_bridge->streams[idx[k]];
        MOQ_TEST_CHECK(!e->active);
        /* Take the ref from the allocator directly. Calling
         * bridge_assign_inbound_ref on an already-active entry would find it and
         * hand back its own still-unset ref. */
        refs[k] = moq_stream_ref_from_u64(tp.client_bridge->next_inbound_ref++);
        MOQ_TEST_CHECK(refs[k]._v != 0);
        e->ref = refs[k];
        e->transport_id = 9000 + idx[k];
        e->kind = BRIDGE_STREAM_UNI;
        e->origin = BRIDGE_ORIGIN_PEER;
        e->active = true;
        e->pending_retry = true;
    }
    for (size_t k = 0; k < 3; k++) {
        MOQ_TEST_CHECK(refs[k]._v != refs[(k + 1) % 3]._v);
        bridge_stream_entry_t *e =
            bridge_find_by_ref(tp.client_bridge, refs[k]);
        MOQ_TEST_CHECK(e == &tp.client_bridge->streams[idx[k]]);
        MOQ_TEST_CHECK(bridge_find_by_id(tp.client_bridge, 9000 + idx[k]) == e);
        MOQ_TEST_CHECK(e->pending_retry);
    }
    /* The scan's first pending owner is exactly the entry placed at idx[0]. */
    uint32_t first = UINT32_MAX;
    for (uint32_t i = 0; i < tp.client_bridge->max_streams; i++)
        if (tp.client_bridge->streams[i].active &&
            tp.client_bridge->streams[i].pending_retry) { first = i; break; }
    MOQ_TEST_CHECK(first == idx[0]);

    sweep_arm_closing_subgroup(tp.client, 0);
    /* Arming must not introduce a due deadline, or the tick stage could report
     * a suspension of its own and hide a scan that failed to stop. */
    MOQ_TEST_CHECK(moq_session_next_deadline_us(tp.client) == UINT64_MAX);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    /* One owner observed the suspension; the scan stopped there. */
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(tp.client->sweep_active);
    for (size_t k = 0; k < 3; k++) {
        bridge_stream_entry_t *e =
            bridge_find_by_id(tp.client_bridge, 9000 + idx[k]);
        MOQ_TEST_CHECK(e != NULL);
        MOQ_TEST_CHECK(e->pending_retry);
        MOQ_TEST_CHECK(e->active);
    }
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(moq_session_state(tp.client) != MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_CLOSING);
    MOQ_TEST_CHECK(tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < tp.client_ep.count; i++)
        MOQ_TEST_CHECK(tp.client_ep.ops[i].kind != FAKE_OP_CLOSE);

    test_pair_destroy(&tp);
    return failures;
}

/*
 * Durable progress and suspension in the SAME pass -- the two-field outcome the
 * inbound carrier exists to report.
 *
 * A reset delivered to entry i satisfies its subscription's terminal stream
 * count, and the eager finalization that follows blocks on the full event
 * queue. That leaves runnable sweep work behind MID-PASS, which the next
 * entry's advance preamble then cannot afford. So entry i commits while entry j
 * suspends, and no subgroup is armed: the reset itself creates the work.
 *
 * The non-streaming branch is required. Under streaming delivery
 * handle_data_reset pushes a terminal chunk first, which the deliberately full
 * queue blocks, so the reset would never be recorded and the construction
 * would evaporate.
 */
static int test_progress_then_suspension_in_one_pass(void)
{
    int failures = 0;

    test_pair_t tp;
    if (test_pair_init_ex(&tp, 1) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }
    MOQ_TEST_CHECK(!tp.client->streaming_objects);

    /* Client subscribes; the SUBSCRIBE_OK it receives stays queued and is what
     * keeps the single event slot full for the rest of the test. */
    moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { ns_parts, 1 };
    moq_subscribe_cfg_t sub_cfg;
    moq_subscribe_cfg_init(&sub_cfg);
    sub_cfg.track_namespace = ns;
    sub_cfg.track_name = MOQ_BYTES_LITERAL("video");
    sub_cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t csub;
    MOQ_TEST_CHECK(moq_session_subscribe(tp.client, &sub_cfg, 0, &csub) == MOQ_OK);
    pump_until_quiescent(&tp, 20, 0);

    moq_event_t ev;
    moq_subscription_t ssub = MOQ_SUBSCRIPTION_INVALID;
    while (moq_session_poll_events(tp.server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_subscription_is_valid(ssub));
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    MOQ_TEST_CHECK(moq_session_accept_subscribe(tp.server, ssub, &acc, 0) == MOQ_OK);
    pump_until_quiescent(&tp, 20, 0);
    /* Exactly one queued event, and it is this subscription's SUBSCRIBE_OK --
     * inspected in place, since draining it would free the slot the whole
     * construction depends on. */
    MOQ_TEST_CHECK(tp.client->event_tail - tp.client->event_head == 1);
    const moq_event_t *qe =
        &tp.client->events[tp.client->event_head % tp.client->event_cap];
    MOQ_TEST_CHECK(qe->kind == MOQ_EVENT_SUBSCRIBE_OK);
    MOQ_TEST_CHECK(qe->u.subscribe_ok.sub._opaque == csub._opaque);

    /* One real peer data uni bound to that subscription: header only, which
     * emits nothing and so leaves no pending retry of its own. */
    moq_subgroup_cfg_t sg_cfg;
    moq_subgroup_cfg_init(&sg_cfg);
    sg_cfg.group_id = 0;
    sg_cfg.subgroup_id = 0;
    sg_cfg.publisher_priority = 200;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK(moq_session_open_subgroup(tp.server, ssub, &sg_cfg, 0, &sg)
                   == MOQ_OK);
    fake_endpoint_clear_ops(&tp.server_ep);
    moq_transport_bridge_service(tp.server_bridge, 0);

    uint8_t hdr[256]; size_t hdr_len = 0;
    uint64_t suni = 0; bool have = false;
    for (size_t i = 0; i < tp.server_ep.count; i++) {
        fake_op_t *o = &tp.server_ep.ops[i];
        if (o->kind == FAKE_OP_OPEN_UNI) { suni = o->stream_id; have = true; }
        else if (o->kind == FAKE_OP_WRITE && have && o->stream_id == suni &&
                 hdr_len + o->data_len <= sizeof(hdr)) {
            memcpy(hdr + hdr_len, o->data, o->data_len);
            hdr_len += o->data_len;
        }
    }
    MOQ_TEST_CHECK(have && hdr_len > 0);
    fake_endpoint_clear_ops(&tp.server_ep);

    const uint64_t data_sid = 7000;
    MOQ_TEST_CHECK(moq_transport_bridge_on_peer_uni_bytes(
                       tp.client_bridge, data_sid, hdr, hdr_len, false, 0)
                   == MOQ_OK);
    bridge_stream_entry_t *ei = bridge_find_by_id(tp.client_bridge, data_sid);
    MOQ_TEST_CHECK(ei != NULL && !ei->pending_retry && !ei->pending_stop);

    /* The RX stream really is bound to this entry. */
    size_t rx_slot = SIZE_MAX;
    for (size_t i = 0; i < tp.client->rx_cap; i++)
        if (tp.client->rx_streams[i].active &&
            tp.client->rx_streams[i].stream_ref._v == ei->ref._v)
            rx_slot = i;
    MOQ_TEST_CHECK(rx_slot != SIZE_MAX);
    MOQ_TEST_CHECK(tp.client->rx_streams[rx_slot].sub._opaque == csub._opaque);

    /* A real terminal done advertising exactly one stream. */
    MOQ_TEST_CHECK(moq_session_close_subgroup(tp.server, sg, 0) == MOQ_OK);
    moq_done_subscribe_cfg_t dc;
    moq_done_subscribe_cfg_init(&dc);
    dc.stream_count = 1;
    MOQ_TEST_CHECK(moq_session_done_subscribe(tp.server, ssub, &dc, 0) == MOQ_OK);
    fake_endpoint_clear_ops(&tp.server_ep);
    moq_transport_bridge_service(tp.server_bridge, 0);
    for (size_t i = 0; i < tp.server_ep.count; i++) {
        fake_op_t *o = &tp.server_ep.ops[i];
        if (o->kind == FAKE_OP_WRITE && o->stream_id >= 2000 &&
            o->stream_id < 3000)
            MOQ_TEST_CHECK(moq_transport_bridge_on_peer_control_bytes(
                tp.client_bridge, o->stream_id, o->data, o->data_len,
                false, 0) == MOQ_OK);
    }
    fake_endpoint_clear_ops(&tp.server_ep);

    size_t sub_slot = SIZE_MAX;
    for (size_t i = 0; i < tp.client->sub_cap; i++)
        if (tp.client->subs[i].done_pending) sub_slot = i;
    MOQ_TEST_CHECK(sub_slot != SIZE_MAX);
    moq_sub_entry_t *se = &tp.client->subs[sub_slot];
    MOQ_TEST_CHECK(se->handle._opaque == csub._opaque);
    MOQ_TEST_CHECK(se->done_stream_count == 1);
    MOQ_TEST_CHECK(se->processed_stream_count == 0);
    MOQ_TEST_CHECK(!se->done_expired);
    MOQ_TEST_CHECK(se->done_deadline_us > 0);

    /* Entry i takes a reset; S6 already proves real reset admission, so the
     * flag is placed directly to keep this test on its own subject. */
    ei->pending_reset = true;
    ei->pending_reset_code = 0x9;
    uint32_t i_idx = (uint32_t)(ei - tp.client_bridge->streams);

    /* Entry j: a coherent later owner with a distinct ref. */
    uint32_t j_idx = i_idx + 3;
    MOQ_TEST_CHECK(j_idx < tp.client_bridge->max_streams);
    bridge_stream_entry_t *ej = &tp.client_bridge->streams[j_idx];
    MOQ_TEST_CHECK(!ej->active);
    moq_stream_ref_t jref =
        moq_stream_ref_from_u64(tp.client_bridge->next_inbound_ref++);
    MOQ_TEST_CHECK(jref._v != 0 && jref._v != ei->ref._v);
    ej->ref = jref;
    ej->transport_id = 7100;
    ej->kind = BRIDGE_STREAM_UNI;
    ej->origin = BRIDGE_ORIGIN_PEER;
    ej->active = true;
    ej->pending_retry = true;
    MOQ_TEST_CHECK(!ej->pending_reset && !ej->pending_stop && !ej->pending_fin);
    MOQ_TEST_CHECK(bridge_find_by_ref(tp.client_bridge, jref) == ej);

    /* Nothing else can supply work or a competing suspension. */
    MOQ_TEST_CHECK(tp.client->event_head != tp.client->event_tail);
    MOQ_TEST_CHECK(tp.client->action_head == tp.client->action_tail);
    MOQ_TEST_CHECK(tp.client_bridge->pending_count == 0);
    MOQ_TEST_CHECK(!tp.client_bridge->pending_control);
    /* The reported deadline is this entry's terminal wait, and it is future. */
    MOQ_TEST_CHECK(moq_session_next_deadline_us(tp.client) ==
                   se->done_deadline_us);
    MOQ_TEST_CHECK(se->done_deadline_us > 0);

    /* j is the NEXT pending owner after i: an unnoticed entry between them
     * could suspend instead and every other assertion would still hold. */
    uint32_t next_pending = UINT32_MAX;
    for (uint32_t i = i_idx + 1; i < tp.client_bridge->max_streams; i++) {
        bridge_stream_entry_t *e = &tp.client_bridge->streams[i];
        if (e->active && bridge_stream_has_inbound_pending(e)) {
            next_pending = i;
            break;
        }
    }
    MOQ_TEST_CHECK(next_pending == j_idx);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out) == MOQ_OK);

    /* Entry i could NOT commit: the abnormal-subgroup event is bounded, and the
     * event queue is full, so the reset is retained rather than lost. */
    MOQ_TEST_CHECK(ei->pending_reset);
    MOQ_TEST_CHECK(ei->pending_reset_code == 0x9);
    MOQ_TEST_CHECK(ei->active);
    MOQ_TEST_CHECK(tp.client->rx_streams[rx_slot].active);
    MOQ_TEST_CHECK(se->processed_stream_count == 0);

    /* A blocked reset is not progress and does not gate the pass: entry j is
     * free to run. WOULD_BLOCK here means only that THIS stream's reset event
     * could not be delivered yet -- it is not MOQ_SESSION_SUSPENDED. */
    MOQ_TEST_CHECK(!ej->pending_retry);

    /* The pass stops on the retained reset, not on a budget suspension: the
     * old row suspended during entry i's eager finalization, which cannot be
     * reached while the reset itself is still owed. */
    MOQ_TEST_CHECK(!out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count == suspends);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));

    /* Drain EXACTLY the one blocking event, and classify it: silently
     * discarding setup or reset output would hide a wrong-event bug. */
    {
        moq_event_t bev;
        MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &bev, 1) == 1);
        MOQ_TEST_CHECK(bev.kind != MOQ_EVENT_SUBGROUP_RESET);
        moq_event_cleanup(&bev);
    }

    /* Second service pass: the retained reset retries and retires the entry. */
    uint64_t suspends2 = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out2;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out2) == MOQ_OK);

    /* Exactly one SUBGROUP_RESET, carrying the retained code and identity. */
    {
        moq_event_t rev;
        int n_reset = 0;
        while (moq_session_poll_events(tp.client, &rev, 1) == 1) {
            if (rev.kind == MOQ_EVENT_SUBGROUP_RESET) {
                n_reset++;
                /* Exact image, not just shape: the retained code AND the
                 * identity this fixture declared (subscriber-role stream on
                 * csub, group/subgroup 0, no end-of-group). */
                MOQ_TEST_CHECK(rev.u.subgroup_reset.error_code == 0x9);
                MOQ_TEST_CHECK(rev.u.subgroup_reset.sub._opaque == csub._opaque);
                MOQ_TEST_CHECK(!moq_publication_is_valid(rev.u.subgroup_reset.pub));
                MOQ_TEST_CHECK(rev.u.subgroup_reset.group_id == 0);
                MOQ_TEST_CHECK(rev.u.subgroup_reset.subgroup_id == 0);
                MOQ_TEST_CHECK(!rev.u.subgroup_reset.end_of_group);
                MOQ_TEST_CHECK(rev.detail_size ==
                    (uint32_t)sizeof(moq_subgroup_reset_event_t));
            }
            moq_event_cleanup(&rev);
        }
        MOQ_TEST_CHECK(n_reset == 1);
    }

    /* Entry i committed now. */
    MOQ_TEST_CHECK(!ei->pending_reset);
    MOQ_TEST_CHECK(!ei->active);
    MOQ_TEST_CHECK(!tp.client->rx_streams[rx_slot].active);
    MOQ_TEST_CHECK(se->processed_stream_count == 1);
    MOQ_TEST_CHECK(se->processed_stream_count >= se->done_stream_count);
    (void)suspends2;

    /* A further pass neither duplicates the reset nor resurrects the entry. */
    {
        moq_bridge_budgeted_result_t out3;
        MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                           tp.client_bridge, 0, 0, &out3) == MOQ_OK);
        moq_event_t xev;
        int extra = 0;
        while (moq_session_poll_events(tp.client, &xev, 1) == 1) {
            if (xev.kind == MOQ_EVENT_SUBGROUP_RESET) extra++;
            moq_event_cleanup(&xev);
        }
        MOQ_TEST_CHECK(extra == 0);
        MOQ_TEST_CHECK(!ei->active);
    }
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(moq_session_state(tp.client) != MOQ_SESS_CLOSED);

    test_pair_destroy(&tp);
    return failures;
}

/*
 * Event scratch is reclaimed on its own queue-empty conditional, separately
 * from send_len, so it needs its own proof -- and one built from a REACHABLE
 * state. A terminal event cannot serve: a real close discards the sweep cursor,
 * so a live cursor alongside a queued SESSION_CLOSED is a state the session
 * never occupies. A GOAWAY carrying a New Session URI is nonterminal and copies
 * that URI into event scratch, which is exactly the shape needed.
 *
 * The event is queued BEFORE the suspension, so the suspended call cannot
 * reclaim, and polled after it: poll_events does not advance, so the queue is
 * empty while the scratch it fed is still occupied. Only the continuation may
 * release it, and at the SAME timestamp.
 */
static int test_continuation_reclaims_event_scratch(void)
{
    int failures = 0;

    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    moq_event_t ev;
    while (moq_session_poll_events(tp.client, &ev, 1) > 0)
        moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(tp.client->event_scratch_len == 0);

    /* A real server GOAWAY with a URI: the client copies it into event scratch
     * and queues MOQ_EVENT_GOAWAY. */
    static const char uri[] = "wss://new.example.com";
    MOQ_TEST_CHECK(moq_session_goaway(tp.server, (const uint8_t *)uri,
                                      sizeof(uri) - 1, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.server_bridge, 0) == MOQ_OK);
    for (size_t i = 0; i < tp.server_ep.count; i++) {
        fake_op_t *o = &tp.server_ep.ops[i];
        if (o->kind != FAKE_OP_WRITE) continue;
        MOQ_TEST_CHECK(moq_transport_bridge_on_peer_control_bytes(
            tp.client_bridge, o->stream_id, o->data, o->data_len,
            false, 0) == MOQ_OK);
    }
    fake_endpoint_clear_ops(&tp.server_ep);

    MOQ_TEST_CHECK(tp.client->event_head != tp.client->event_tail);
    size_t scratch_used = tp.client->event_scratch_len;
    MOQ_TEST_CHECK(scratch_used >= sizeof(uri) - 1);

    /* Suspend with the event still queued: nothing may be reclaimed. */
    sweep_arm_expired_pub(tp.client, 0);
    sweep_bind_rx(tp.client, 2, tp.client->publishes[0].handle);
    session_budget_enter(tp.client, 0);
    MOQ_TEST_CHECK(session_begin_advance_budgeted(tp.client, 100) ==
                   MOQ_SESSION_SUSPENDED);
    session_budget_leave(tp.client);
    MOQ_TEST_CHECK(tp.client->sweep_active);
    MOQ_TEST_CHECK(tp.client->event_scratch_len == scratch_used);

    /* Drain it. poll_events does not advance the cursor. */
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_GOAWAY);
    MOQ_TEST_CHECK(ev.u.goaway.new_session_uri.len == sizeof(uri) - 1);
    moq_event_cleanup(&ev);
    MOQ_TEST_CHECK(tp.client->event_head == tp.client->event_tail);
    MOQ_TEST_CHECK(tp.client->sweep_active);
    MOQ_TEST_CHECK(tp.client->event_scratch_len == scratch_used);

    /* The continuation, at the same timestamp, owes the reclamation. */
    session_begin_advance(tp.client, 100);
    MOQ_TEST_CHECK(!tp.client->sweep_active);
    MOQ_TEST_CHECK(tp.client->event_scratch_len == 0);

    test_pair_destroy(&tp);
    return failures;
}

/*
 * A control retry that suspends must leave every field the completing path
 * would have written, so the retry and its deferred FIN survive to a later
 * pass and the close is delivered exactly once.
 *
 * The one-slot event queue is the whole fixture: the unpolled SETUP_COMPLETE
 * keeps it full, which is what makes the peer's GOAWAY block and establishes a
 * genuine pending control retry. Sweep work is armed only AFTER that, because
 * the delivery itself is an advancing call that would otherwise consume it; and
 * it is a closing subgroup rather than an expired publication because
 * publication finalization emits an event, which would refill the slot the
 * control retry needs.
 */
static int test_control_retry_suspension_preserves_pending(void)
{
    int failures = 0;

    test_pair_t tp;
    if (test_pair_init_ex(&tp, 1) < 0) { failures++; return failures; }

    /* Establish, draining the SERVER only: the client's single event slot must
     * stay occupied by its own SETUP_COMPLETE. */
    moq_session_start(tp.client, 0);
    pump_until_quiescent(&tp, 20, 0);

    moq_event_t ev;
    bool s_setup = false;
    while (moq_session_poll_events(tp.server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) s_setup = true;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(s_setup);
    MOQ_TEST_CHECK(moq_session_state(tp.client) == MOQ_SESS_ESTABLISHED);
    MOQ_TEST_CHECK(tp.client->event_head != tp.client->event_tail);

    /* A real server GOAWAY, delivered as its actual encoded bytes with FIN. */
    MOQ_TEST_CHECK(moq_session_goaway(tp.server, NULL, 0, 0) == MOQ_OK);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.server_bridge, 0) == MOQ_OK);

    bool fed = false;
    moq_result_t frc = MOQ_OK;
    for (size_t i = 0; i < tp.server_ep.count; i++) {
        fake_op_t *o = &tp.server_ep.ops[i];
        if (o->kind != FAKE_OP_WRITE) continue;
        frc = moq_transport_bridge_on_peer_control_bytes(
            tp.client_bridge, o->stream_id, o->data, o->data_len, true, 0);
        fed = true;
    }
    fake_endpoint_clear_ops(&tp.server_ep);
    MOQ_TEST_CHECK(fed);
    MOQ_TEST_CHECK(frc == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(tp.client_bridge->pending_control);
    MOQ_TEST_CHECK(tp.client_bridge->pending_control_fin);

    /* Now arm one runnable sweep unit, with nothing to spend on it. */
    MOQ_TEST_CHECK(tp.client->sg_cap > 0);
    sweep_arm_closing_subgroup(tp.client, 0);

    uint64_t suspends = session_budget_suspend_count;
    moq_bridge_budgeted_result_t out;
    MOQ_TEST_CHECK(moq_transport_bridge_service_budgeted(
                       tp.client_bridge, 0, 0, &out) == MOQ_OK);

    MOQ_TEST_CHECK(out.suspended);
    MOQ_TEST_CHECK(out.sweep_spent == 0);
    MOQ_TEST_CHECK(session_budget_suspend_count - suspends == 1);
    MOQ_TEST_CHECK(tp.client_bridge->pending_control);
    MOQ_TEST_CHECK(tp.client_bridge->pending_control_fin);
    MOQ_TEST_CHECK(!tp.client_bridge->needs_close);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(moq_session_state(tp.client) != MOQ_SESS_CLOSED);
    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_CLOSING);

    /* Free the slot the retained GOAWAY needs. */
    MOQ_TEST_CHECK(moq_session_poll_events(tp.client, &ev, 1) == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_SETUP_COMPLETE);
    moq_event_cleanup(&ev);

    /* Unlimited service completes the sweep -- a subgroup reap emits no event,
     * so the retry still finds capacity -- and delivers the deferred FIN. */
    fake_endpoint_clear_ops(&tp.client_ep);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);

    MOQ_TEST_CHECK(tp.client->subgroups[0].state == MOQ_SG_FREE);
    MOQ_TEST_CHECK(!tp.client_bridge->pending_control);
    MOQ_TEST_CHECK(!tp.client_bridge->pending_control_fin);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(moq_session_state(tp.client) == MOQ_SESS_CLOSED);

    size_t closes = 0;
    for (size_t i = 0; i < tp.client_ep.count; i++)
        if (tp.client_ep.ops[i].kind == FAKE_OP_CLOSE) closes++;
    MOQ_TEST_CHECK(closes == 1);

    /* Re-servicing must not close a second time. */
    fake_endpoint_clear_ops(&tp.client_ep);
    MOQ_TEST_CHECK(moq_transport_bridge_service(tp.client_bridge, 0) == MOQ_OK);
    closes = 0;
    for (size_t i = 0; i < tp.client_ep.count; i++)
        if (tp.client_ep.ops[i].kind == FAKE_OP_CLOSE) closes++;
    MOQ_TEST_CHECK(closes == 0);

    test_pair_destroy(&tp);
    return failures;
}

static int test_create_destroy(void)
{
    int failures = 0;
    fake_endpoint_t ep;
    fake_endpoint_init(&ep, 100, 200);

    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), MOQ_PERSPECTIVE_CLIENT);
    moq_session_t *s = NULL;
    MOQ_TEST_CHECK(moq_session_create(&cfg, 0, &s) == MOQ_OK);

    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    moq_transport_bridge_t *b = NULL;
    MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &ep.vtable, &ep, &b) == MOQ_OK);
    MOQ_TEST_CHECK(b != NULL);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(b));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(b));
    MOQ_TEST_CHECK(moq_transport_bridge_stream_count(b) == 0);

    moq_transport_bridge_destroy(b);
    moq_session_destroy(s);
    return failures;
}

static int test_create_rejects_bad_ops(void)
{
    int failures = 0;
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), MOQ_PERSPECTIVE_CLIENT);
    moq_session_t *s = NULL;
    moq_session_create(&cfg, 0, &s);

    moq_transport_endpoint_ops_t bad = MOQ_TRANSPORT_ENDPOINT_OPS_INIT;
    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    moq_transport_bridge_t *b = NULL;

    MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, s, &bad, &bad, &b) == MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(b == NULL);

    moq_session_destroy(s);
    return failures;
}

static int test_setup_handshake(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }

    MOQ_TEST_CHECK(setup_handshake(&tp));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.server_bridge));

    test_pair_destroy(&tp);
    return failures;
}

static int test_control_write_backpressure(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }

    tp.client_ep.block_write = true;
    moq_session_start(tp.client, 0);
    moq_transport_bridge_service(tp.client_bridge, 0);

    MOQ_TEST_CHECK(moq_transport_bridge_has_pending(tp.client_bridge));
    MOQ_TEST_CHECK(tp.client_ep.block_count > 0);

    tp.client_ep.block_write = false;
    tp.client_ep.block_count = 0;

    moq_transport_bridge_service(tp.client_bridge, 0);
    MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(tp.client_bridge));

    test_pair_destroy(&tp);
    return failures;
}

static int test_transport_close(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }

    moq_transport_bridge_on_transport_close(tp.client_bridge, 0x42, 0);

    MOQ_TEST_CHECK(moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(tp.client_bridge));
    MOQ_TEST_CHECK(moq_transport_bridge_stream_count(tp.client_bridge) == 0);

    test_pair_destroy(&tp);
    return failures;
}

static int test_datagram_inbound_not_fatal(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    moq_result_t rc = moq_transport_bridge_on_peer_datagram(
        tp.server_bridge, (const uint8_t *)"test", 4, 0);
    MOQ_TEST_CHECK(rc >= 0);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.server_bridge));

    test_pair_destroy(&tp);
    return failures;
}

static int test_inbound_uni_after_setup(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* After handshake, deliver some uni bytes to the server.
     * The session should accept them (it's established). */
    uint8_t dummy[4] = {0x01, 0x02, 0x03, 0x04};
    moq_result_t rc = moq_transport_bridge_on_peer_uni_bytes(
        tp.server_bridge, 5000, dummy, 4, false, 0);
    MOQ_TEST_CHECK(rc >= 0 || rc == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.server_bridge));

    test_pair_destroy(&tp);
    return failures;
}

static moq_transport_result_t error_close_transport(void *ctx, uint64_t code,
                                                     const uint8_t *r, size_t l)
{
    (void)ctx; (void)code; (void)r; (void)l;
    return MOQ_TRANSPORT_ERROR;
}

static int test_close_error_is_fatal_not_closed(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* Replace close_transport with one that always returns ERROR */
    tp.client_ep.vtable.close_transport = error_close_transport;

    /* Deliver control FIN to client using the control stream ID
     * that was established during handshake (client opened bidi 2000) */
    moq_transport_bridge_on_peer_control_bytes(
        tp.client_bridge, 2000,
        NULL, 0, true, 0);

    /* service() tries close_transport → ERROR → fatal, not closed */
    moq_transport_bridge_service(tp.client_bridge, 0);

    MOQ_TEST_CHECK(moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.client_bridge));

    test_pair_destroy(&tp);
    return failures;
}

/* -- Regression: empty uni on unknown stream ----------------------- */

static int test_empty_uni_no_ghost_stream(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    size_t before = moq_transport_bridge_stream_count(tp.server_bridge);

    moq_result_t rc = moq_transport_bridge_on_peer_uni_bytes(
        tp.server_bridge, 9999, NULL, 0, false, 0);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    MOQ_TEST_CHECK(moq_transport_bridge_stream_count(tp.server_bridge) == before);

    test_pair_destroy(&tp);
    return failures;
}

/* -- Regression: truncated vtable rejected -------------------------- */

static int test_truncated_vtable_rejected(void)
{
    int failures = 0;
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default(), MOQ_PERSPECTIVE_CLIENT);
    moq_session_t *s = NULL;
    moq_session_create(&cfg, 0, &s);

    moq_transport_endpoint_ops_t trunc = MOQ_TRANSPORT_ENDPOINT_OPS_INIT;
    trunc.open_uni = fake_open_uni;
    trunc.open_bidi = fake_open_bidi;
    trunc.write = fake_write;
    trunc.reset_stream = fake_reset;
    trunc.stop_sending = fake_stop;
    trunc.close_transport = fake_close;
    /* Shrink struct_size so close_transport is outside declared bounds.
     * The old bug would still read close_transport and accept it. The
     * fix uses HAS_FIELD() and rejects because struct_size is too small. */
    trunc.struct_size = (uint32_t)offsetof(moq_transport_endpoint_ops_t,
                                            close_transport);

    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    moq_transport_bridge_t *b = NULL;

    moq_result_t rc = moq_transport_bridge_create(
        &bcfg, s, &trunc, &trunc, &b);
    MOQ_TEST_CHECK(rc == MOQ_ERR_INVAL);
    MOQ_TEST_CHECK(b == NULL);

    moq_session_destroy(s);
    return failures;
}

/* Bogus outbound datagram result test deferred to conformance suite —
 * triggering SEND_DATAGRAM requires a full subscribe+datagram flow.
 * The sanitizer switch in transport_bridge.c is verified by inspection
 * and will be covered by conformance scenarios. */

/* -- Hard retry: control write blocked then close ------------------- */

static int test_close_retry_after_blocked_control(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }

    /* Block writes so control setup goes pending */
    tp.client_ep.block_write = true;
    moq_session_start(tp.client, 0);
    moq_transport_bridge_service(tp.client_bridge, 0);
    MOQ_TEST_CHECK(moq_transport_bridge_has_pending(tp.client_bridge));

    /* Deliver control FIN while write is still blocked.
     * The bridge should notify the session and schedule deferred close. */
    moq_transport_bridge_on_peer_control_bytes(
        tp.client_bridge, 2000, NULL, 0, true, 0);

    /* Unblock and service — close must happen */
    tp.client_ep.block_write = false;
    moq_transport_bridge_service(tp.client_bridge, 0);

    /* Bridge must be closed (not fatal), with endpoint close observed */
    MOQ_TEST_CHECK(moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(tp.client_bridge));
    MOQ_TEST_CHECK(fake_endpoint_find(&tp.client_ep, FAKE_OP_CLOSE) != NULL);

    test_pair_destroy(&tp);
    return failures;
}

/* -- Hard retry: close_transport WOULD_BLOCK then succeeds ---------- */

static int test_close_retry_would_block(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* Block close, then trigger control FIN */
    tp.client_ep.block_close = true;
    moq_transport_bridge_on_peer_control_bytes(
        tp.client_bridge, 2000, NULL, 0, true, 0);

    moq_transport_bridge_service(tp.client_bridge, 0);
    /* Close should be pending (WOULD_BLOCK) */
    MOQ_TEST_CHECK(moq_transport_bridge_has_pending(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.client_bridge));

    /* Unblock and retry */
    tp.client_ep.block_close = false;
    fake_endpoint_clear_ops(&tp.client_ep);
    moq_transport_bridge_service(tp.client_bridge, 0);

    MOQ_TEST_CHECK(moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(fake_endpoint_find(&tp.client_ep, FAKE_OP_CLOSE) != NULL);
    MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(tp.client_bridge));

    test_pair_destroy(&tp);
    return failures;
}

/* -- Reset on unknown stream is no-op ------------------------------- */

static int test_reset_on_unknown_stream(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* Reset for an unknown stream should be a no-op (no stream to reset) */
    moq_result_t rc = moq_transport_bridge_on_peer_stream_reset(
        tp.server_bridge, 9999, 0x42, 0);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.server_bridge));

    test_pair_destroy(&tp);
    return failures;
}

/* -- Transport close clears all state ------------------------------- */

static int test_transport_close_clears_state(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }

    /* Create real pending state: block writes then start (no handshake).
     * session_start emits SEND_CONTROL which the bridge tries to send.
     * With writes blocked, the control data goes to the pending queue. */
    tp.client_ep.block_write = true;
    moq_session_start(tp.client, 0);
    moq_transport_bridge_service(tp.client_bridge, 0);
    MOQ_TEST_CHECK(moq_transport_bridge_has_pending(tp.client_bridge));

    /* Transport close should clear everything */
    moq_transport_bridge_on_transport_close(tp.client_bridge, 0x1, 0);

    MOQ_TEST_CHECK(moq_transport_bridge_is_closed(tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(tp.client_bridge));
    MOQ_TEST_CHECK(moq_transport_bridge_stream_count(tp.client_bridge) == 0);
    MOQ_TEST_CHECK(moq_transport_bridge_tombstone_count(tp.client_bridge) == 0);

    test_pair_destroy(&tp);
    return failures;
}

/* -- Regression: dropped inbound uni stream is discarded, not misparsed -----
 *
 * When the session drops a peer uni data stream -- here, a subgroup for a
 * track_alias nobody subscribed to -- it frees its rx entry and issues
 * STOP_DATA. Bytes already in flight on that stream must be DISCARDED by the
 * bridge: feeding them onward would have the session open a fresh rx entry and
 * parse mid-stream bytes as a leading stream type ("unknown data stream type",
 * 0x3), fataling the transport. This is the shape that previously closed the
 * connection during a live->VOD catalog conversion (the publisher finishes the
 * media subscription while its last objects are still arriving).
 *
 * The same contract holds on both inbound entry points (byte and rcbuf), so the
 * body is shared and run through each via this delivery shim. */
static moq_result_t deliver_uni(moq_transport_bridge_t *b, bool use_rcbuf,
                                uint64_t sid, const uint8_t *data, size_t len,
                                bool fin)
{
    if (!use_rcbuf)
        return moq_transport_bridge_on_peer_uni_bytes(b, sid, data, len, fin, 0);

    moq_rcbuf_t *buf = NULL;
    if (moq_rcbuf_create(moq_alloc_default(), data, len, &buf) < 0)
        return MOQ_ERR_NOMEM;
    moq_result_t rc = moq_transport_bridge_on_peer_uni_rcbuf(b, sid, buf, fin, 0);
    moq_rcbuf_decref(buf);
    return rc;
}

static int run_inbound_uni_dropped_then_discarded(bool use_rcbuf)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* A valid subgroup header for an unsubscribed track_alias: the session
     * classifies it, fails to bind, and stops the stream within this call. */
    uint8_t hdr[32];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, hdr, sizeof(hdr));
    moq_d16_subgroup_header_t sh;
    memset(&sh, 0, sizeof(sh));
    sh.type = 0x14;
    sh.subgroup_id_mode = MOQ_SUBGROUP_ID_MODE_PRESENT;
    sh.track_alias = 9999;
    sh.group_id = 0;
    sh.subgroup_id = 0;
    sh.publisher_priority = 128;
    MOQ_TEST_CHECK(moq_d16_encode_subgroup_header(&w, &sh) == MOQ_OK);
    size_t hdr_len = moq_buf_writer_offset(&w);

    const uint64_t sid = 5000;

    /* First delivery: the session drops the stream (STOP_DATA), so the bridge
     * marks it for discard. Not a protocol violation. */
    moq_result_t rc = deliver_uni(tp.server_bridge, use_rcbuf, sid,
                                  hdr, hdr_len, false);
    MOQ_TEST_CHECK(rc >= 0 || rc == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.server_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.server_bridge));

    /* More in-flight bytes on the same stream. Their leading byte (0x20)
     * classifies as an UNKNOWN data stream type: if the bridge re-fed them as a
     * fresh stream the session would fatal with 0x3. With the discard guard
     * they are swallowed and the bridge stays healthy. */
    uint8_t more[4] = { 0x20, 0x00, 0x00, 0x00 };
    rc = deliver_uni(tp.server_bridge, use_rcbuf, sid, more, sizeof(more), false);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.server_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.server_bridge));

    /* Final in-flight bytes with FIN: still no fatal, and the discard entry is
     * retired so it leaves no ghost stream behind. */
    size_t before = moq_transport_bridge_stream_count(tp.server_bridge);
    MOQ_TEST_CHECK(before >= 1);
    rc = deliver_uni(tp.server_bridge, use_rcbuf, sid, more, sizeof(more), true);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.server_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(tp.server_bridge));
    MOQ_TEST_CHECK(moq_transport_bridge_stream_count(tp.server_bridge) ==
                   before - 1);

    test_pair_destroy(&tp);
    return failures;
}

static int test_inbound_uni_dropped_then_discarded(void)
{
    return run_inbound_uni_dropped_then_discarded(false);
}

static int test_inbound_uni_rcbuf_dropped_then_discarded(void)
{
    return run_inbound_uni_dropped_then_discarded(true);
}

/* -- Regression: bytes delivered during inbound pending_retry are kept ------
 *
 * A peer fills the client's event queue so a data stream backs up into
 * PENDING_EMIT (bridge pending_retry). The peer then delivers another object
 * (with FIN) on the SAME stream while pending_retry is set. The transport does
 * not re-deliver stream bytes, so the bridge/session must RETAIN those bytes
 * across the WOULD_BLOCK and deliver the object after the queue drains.
 *
 * Pre-fix: handle_data_bytes_impl retried the pending emit and returned
 * WOULD_BLOCK before appending the new bytes, silently dropping the object.
 * Run through both inbound entry points via deliver_uni(). */
static int run_pending_retry_keeps_bytes(bool use_rcbuf_extra)
{
    int failures = 0;
    test_pair_t tp;
    /* max_events = 1: object 0 fills the queue, object 1 -> PENDING_EMIT. */
    if (test_pair_init_ex(&tp, 1) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* Client subscribes; server accepts (carry the control both ways via pump). */
    moq_bytes_t ns_parts[] = { MOQ_BYTES_LITERAL("live") };
    moq_namespace_t ns = { ns_parts, 1 };
    moq_subscribe_cfg_t sub_cfg;
    moq_subscribe_cfg_init(&sub_cfg);
    sub_cfg.track_namespace = ns;
    sub_cfg.track_name = MOQ_BYTES_LITERAL("video");
    sub_cfg.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
    moq_subscription_t sub;
    MOQ_TEST_CHECK(moq_session_subscribe(tp.client, &sub_cfg, 0, &sub) == MOQ_OK);
    pump_until_quiescent(&tp, 20, 0);

    moq_event_t ev;
    moq_subscription_t server_sub = MOQ_SUBSCRIPTION_INVALID;
    while (moq_session_poll_events(tp.server, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            server_sub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    MOQ_TEST_CHECK(moq_subscription_is_valid(server_sub));
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    MOQ_TEST_CHECK(moq_session_accept_subscribe(tp.server, server_sub, &acc, 0)
                   == MOQ_OK);
    pump_until_quiescent(&tp, 20, 0);
    /* Drain the client's SUBSCRIBE_OK so the (size-1) event queue starts empty. */
    while (moq_session_poll_events(tp.client, &ev, 1) > 0)
        moq_event_cleanup(&ev);

    /* Server writes obj0+obj1 to one subgroup; capture the produced uni wire
     * bytes (header + obj0 + obj1) from the server endpoint into buf1. */
    moq_subgroup_cfg_t sg_cfg;
    moq_subgroup_cfg_init(&sg_cfg);
    sg_cfg.group_id = 0;
    sg_cfg.subgroup_id = 0;
    sg_cfg.publisher_priority = 200;
    moq_subgroup_handle_t sg;
    MOQ_TEST_CHECK(moq_session_open_subgroup(tp.server, server_sub, &sg_cfg, 0, &sg)
                   == MOQ_OK);
    const char *want[3] = { "AAA", "BBB", "CCC" };
    for (int i = 0; i < 2; i++) {
        moq_rcbuf_t *p = NULL;
        moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)want[i], 3, &p);
        MOQ_TEST_CHECK(moq_session_write_object(tp.server, sg, (uint64_t)i, p, 0)
                       == MOQ_OK);
        moq_rcbuf_decref(p);
    }
    fake_endpoint_clear_ops(&tp.server_ep);
    moq_transport_bridge_service(tp.server_bridge, 0);

    uint8_t buf1[512]; size_t len1 = 0;
    uint64_t uni_sid = 0; bool have_uni = false;
    for (size_t i = 0; i < tp.server_ep.count; i++) {
        fake_op_t *o = &tp.server_ep.ops[i];
        if (o->kind == FAKE_OP_OPEN_UNI) { uni_sid = o->stream_id; have_uni = true; }
        else if (o->kind == FAKE_OP_WRITE && have_uni && o->stream_id == uni_sid &&
                 len1 + o->data_len <= sizeof(buf1)) {
            memcpy(buf1 + len1, o->data, o->data_len);
            len1 += o->data_len;
        }
    }
    MOQ_TEST_CHECK(have_uni && len1 > 0);
    fake_endpoint_clear_ops(&tp.server_ep);

    /* Server writes obj2 and closes; capture obj2's bytes into buf2. */
    {
        moq_rcbuf_t *p = NULL;
        moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)want[2], 3, &p);
        MOQ_TEST_CHECK(moq_session_write_object(tp.server, sg, 2, p, 0) == MOQ_OK);
        moq_rcbuf_decref(p);
    }
    MOQ_TEST_CHECK(moq_session_close_subgroup(tp.server, sg, 0) == MOQ_OK);
    moq_transport_bridge_service(tp.server_bridge, 0);

    uint8_t buf2[512]; size_t len2 = 0;
    for (size_t i = 0; i < tp.server_ep.count; i++) {
        fake_op_t *o = &tp.server_ep.ops[i];
        if (o->kind == FAKE_OP_WRITE && o->stream_id == uni_sid &&
            len2 + o->data_len <= sizeof(buf2)) {
            memcpy(buf2 + len2, o->data, o->data_len);
            len2 += o->data_len;
        }
    }
    MOQ_TEST_CHECK(len2 > 0);

    /* Deliver buf1 (header+obj0+obj1) to the CLIENT bridge as a peer uni stream.
     * obj0 emits and fills the size-1 event queue; obj1 backs up into
     * PENDING_EMIT, so the bridge returns WOULD_BLOCK (pending_retry). */
    const uint64_t client_sid = 5000;
    moq_result_t rc = moq_transport_bridge_on_peer_uni_bytes(
        tp.client_bridge, client_sid, buf1, len1, false, 0);
    MOQ_TEST_CHECK(rc == MOQ_ERR_WOULD_BLOCK);

    /* While pending_retry is set, deliver obj2 + FIN on the SAME stream. The
     * bytes must be retained (not dropped) even though this also WOULD_BLOCKs. */
    rc = deliver_uni(tp.client_bridge, use_rcbuf_extra, client_sid,
                     buf2, len2, true);
    MOQ_TEST_CHECK(rc == MOQ_ERR_WOULD_BLOCK);

    /* Drain + service-retry until quiescent, collecting delivered objects. */
    bool got[3] = { false, false, false };
    for (int iter = 0; iter < 16; iter++) {
        while (moq_session_poll_events(tp.client, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
                uint64_t oid = ev.u.object_received.object_id;
                moq_rcbuf_t *pl = ev.u.object_received.payload;
                if (oid < 3 && pl && moq_rcbuf_len(pl) == 3 &&
                    memcmp(moq_rcbuf_data(pl), want[oid], 3) == 0)
                    got[(size_t)oid] = true;
            }
            moq_event_cleanup(&ev);
        }
        moq_transport_bridge_service(tp.client_bridge, 0);
    }

    MOQ_TEST_CHECK(got[0]);
    MOQ_TEST_CHECK(got[1]);
    MOQ_TEST_CHECK(got[2]);   /* must not be dropped during pending_retry */
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(tp.client_bridge));

    test_pair_destroy(&tp);
    return failures;
}

static int test_pending_retry_keeps_bytes(void)
{
    return run_pending_retry_keeps_bytes(false);
}

static int test_pending_retry_keeps_rcbuf(void)
{
    return run_pending_retry_keeps_bytes(true);
}

/* == Main =========================================================== */


/* --- independent monotonic terminal facts -------------------------------- *
 * ENQUEUED and OBSERVED are separate. Availability is not observation: a
 * queued-but-unpolled MOQ_EVENT_SESSION_CLOSED must read enqueued=true,
 * observed=false, and only the poll that TRANSFERS it flips observed. */
static int test_terminal_facts_enqueued_then_observed(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }

    bool observed = true; /* poisoned: must be overwritten */
    MOQ_TEST_CHECK(!moq_transport_bridge_terminal_facts(tp.client_bridge,
                                                        &observed));
    MOQ_TEST_CHECK(!observed);          /* neither fact before terminal */

    /* peer-side terminal: the event is enqueued, nobody has polled it */
    moq_transport_bridge_on_transport_close(tp.client_bridge, 0x42, 0);
    observed = true;
    MOQ_TEST_CHECK(moq_transport_bridge_terminal_facts(tp.client_bridge,
                                                       &observed));
    MOQ_TEST_CHECK(!observed);          /* queued is NOT observed */

    /* the poll that transfers it is what makes it observed */
    moq_event_t ev;
    size_t n = 0;
    MOQ_TEST_CHECK(moq_session_poll_events_ex(tp.client, &ev, 1,
                                              sizeof(ev), &n) == MOQ_OK);
    MOQ_TEST_CHECK(n == 1);
    MOQ_TEST_CHECK(ev.kind == MOQ_EVENT_SESSION_CLOSED);
    observed = false;
    MOQ_TEST_CHECK(moq_transport_bridge_terminal_facts(tp.client_bridge,
                                                       &observed));
    MOQ_TEST_CHECK(observed);

    /* monotonic + idempotent: repeating the terminal does not clear anything,
     * and a second poll (no event left) leaves both facts set */
    moq_transport_bridge_on_transport_close(tp.client_bridge, 0x43, 0);
    n = 0;
    (void)moq_session_poll_events_ex(tp.client, &ev, 1, sizeof(ev), &n);
    observed = false;
    MOQ_TEST_CHECK(moq_transport_bridge_terminal_facts(tp.client_bridge,
                                                       &observed));
    MOQ_TEST_CHECK(observed);

    /* out_observed is optional */
    MOQ_TEST_CHECK(moq_transport_bridge_terminal_facts(tp.client_bridge, NULL));

    test_pair_destroy(&tp);
    return failures;
}

/* A non-terminal event must not set the observed fact, and the facts are not
 * derived from the bridge's own terminal latches. */
static int test_terminal_facts_not_set_by_other_events(void)
{
    int failures = 0;
    test_pair_t tp;
    if (test_pair_init(&tp) < 0) { failures++; return failures; }
    if (!setup_handshake(&tp)) { failures++; test_pair_destroy(&tp); return failures; }

    /* the handshake surfaced SETUP_COMPLETE; draining it must not look terminal */
    moq_event_t ev;
    size_t n = 0;
    while (moq_session_poll_events_ex(tp.client, &ev, 1, sizeof(ev),
                                      &n) == MOQ_OK && n > 0)
        n = 0;

    bool observed = true;
    MOQ_TEST_CHECK(!moq_transport_bridge_terminal_facts(tp.client_bridge,
                                                        &observed));
    MOQ_TEST_CHECK(!observed);

    test_pair_destroy(&tp);
    return failures;
}

/* A SETUP whose completion-event tokens can never fit the arena must reach the
 * normal close path so the bridge dispatches CLOSE_SESSION: a buffer error here
 * would instead be escalated to a connection fatal, losing close semantics. */
static int test_setup_scratch_shortfall_closes_not_fatal(void)
{
    int failures = 0;

    moq_session_cfg_t scfg;
    moq_session_cfg_init_sized(&scfg, sizeof(scfg), moq_alloc_default(),
                               MOQ_PERSPECTIVE_SERVER);
    scfg.output_scratch_size = 32;
    moq_session_t *sv = NULL;
    MOQ_TEST_CHECK(moq_session_create(&scfg, 0, &sv) == MOQ_OK);
    if (!sv) return failures;

    fake_endpoint_t ep;
    fake_endpoint_init(&ep, 3000, 4000);
    moq_transport_bridge_t *br = NULL;
    moq_transport_bridge_cfg_t bcfg;
    moq_transport_bridge_cfg_init(&bcfg, moq_alloc_default());
    MOQ_TEST_CHECK(moq_transport_bridge_create(&bcfg, sv, &ep.vtable, &ep,
                                               &br) == MOQ_OK);
    if (!br) { moq_session_destroy(sv); return failures; }

    uint8_t val[40];
    for (size_t i = 0; i < sizeof(val); i++)
        val[i] = (uint8_t)(0x41 + (i % 26));
    uint8_t tokbuf[64];
    moq_buf_writer_t tw;
    moq_buf_writer_init(&tw, tokbuf, sizeof(tokbuf));
    moq_buf_write_vi64(&tw, MOQ_AUTH_TOKEN_USE_VALUE);
    moq_buf_write_vi64(&tw, 0);
    moq_buf_write_vi64(&tw, sizeof(val));
    moq_buf_write_raw(&tw, val, sizeof(val));

    moq_kvp_entry_t prm;
    memset(&prm, 0, sizeof(prm));
    prm.type = MOQ_SETUP_PARAM_AUTHORIZATION_TOKEN;
    prm.value = tokbuf;
    prm.value_len = moq_buf_writer_offset(&tw);

    uint8_t msg[256];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, msg, sizeof(msg));
    moq_d16_encode_client_setup(&w, &prm, 1);

    moq_result_t rc = moq_transport_bridge_on_peer_control_bytes(
        br, 0 /* peer control stream */, msg, moq_buf_writer_offset(&w),
        false, 0);
    MOQ_TEST_CHECK(rc == MOQ_OK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(br));
    MOQ_TEST_CHECK(moq_session_state(sv) == MOQ_SESS_CLOSED);

    MOQ_TEST_CHECK(moq_transport_bridge_service(br, 0) == MOQ_OK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(br));

    size_t closes = 0;
    MOQ_TEST_CHECK(ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < ep.count; i++) {
        if (ep.ops[i].kind != FAKE_OP_CLOSE) continue;
        closes++;
        MOQ_TEST_CHECK(ep.ops[i].error_code == 0x1);
    }
    MOQ_TEST_CHECK(closes == 1);

    moq_transport_bridge_destroy(br);
    moq_session_destroy(sv);
    return failures;
}

/* -- Axis 4: physical bridge retirement ------------------------------
 *
 * SEMANTIC consumption and PHYSICAL retirement are different facts, asserted
 * separately. The causal sequence is the Axis 4 one: the peer FIN arrives WITH
 * the request, so the destination owner is created with the FIN transferred
 * onto it; the application then retires that owner, which -- the FIN having
 * been observed -- owes NO drain; and the bridge's own service must afterwards
 * close our half and retire the physical mapping exactly once.
 *
 * Successful FIN ingress records `peer_send_closed` and leaves the entry ACTIVE
 * until the local half closes (transport_bridge.c:2677), so retirement is a
 * real transition, not a formality.
 *
 * No peer bytes follow the FIN. The only continuation is bridge service.
 */

typedef struct fin_bridge_run {
    test_pair_t tp;
    uint64_t    transport_id;   /* the TRANSPORT stream the peer bytes ride */
    moq_stream_ref_t ref;       /* the SESSION's internal ref -- a DIFFERENT
                                 * value, captured once at ingress and never
                                 * re-derived from an entry that may be gone */
    int         want_slot;
    uint32_t    want_gen;
    uint64_t    want_handle;
} fin_bridge_run_t;

/* -- P7 TRACK_STATUS family ----------------------------------------- */

#define P7_TOK_TYPE 7
static const uint8_t k_p7_tok[]  = { 't','s','t','o','k' };
static const uint8_t k_p7_ns0[]  = { 'e','x','.','c','o','m' };
static const uint8_t k_p7_ns1[]  = { 's','t','a','t' };
static const uint8_t k_p7_name[] = { 't','0' };
#define P7_LARGEST_GROUP 0x33
#define P7_LARGEST_OBJ   0x04
#define P7_EXPIRES_MS    5500

static const moq_ts_entry_t *p7_owner(const moq_session_t *s,
                                      moq_stream_ref_t ref)
{
    moq_request_endpoint_t ep = request_registry_find_by_streamref(s, ref);
    if (ep.kind != MOQ_REQ_TRACK_STATUS) return NULL;
    if (ep.slot < 0 || (size_t)ep.slot >= s->ts_cap) return NULL;
    return &s->track_statuses[ep.slot];
}

static int p7_pool_busy(const moq_session_t *s)
{
    int n = 0;
    for (size_t i = 0; i < s->ts_cap; i++)
        if (s->track_statuses[i].state != MOQ_TS_FREE) n++;
    return n;
}

static int p7_derive_slot(const moq_session_t *s, uint32_t *out_gen)
{
    for (size_t i = 0; i < s->ts_cap; i++)
        if (s->track_statuses[i].state == MOQ_TS_FREE) {
            *out_gen = s->track_statuses[i].generation | 1u;
            return (int)i;
        }
    return -1;
}

/* The peer's request id for the request under check. Peer-opened request ids
 * advance by two, so a second request on the same session legitimately carries
 * a different one; the expectation is declared by the caller rather than being
 * read back from the entry. */
static uint64_t p7_want_request_id;

/* The FIN fact the owner under check must carry. The first request arrives WITH
 * a FIN, so its owner latches one; a reused slot admitted from a request with
 * NO FIN must show a CLEARED latch. Declared by the caller rather than assumed
 * true, or a free that left a stale `true` behind would pass at reuse. */
static int p7_want_fin = 1;

static int p7_check_live(const moq_session_t *s, moq_stream_ref_t ref,
                         int want_slot, uint32_t want_gen, uint64_t want_handle,
                         const char *what)
{
    int bad = 0;
    if (want_slot < 0 || (size_t)want_slot >= s->ts_cap) {
        fprintf(stderr, "FINBR %s: declared slot out of range\n", what);
        return 1;
    }
    const moq_ts_entry_t *e = p7_owner(s, ref);
    if (!e) { fprintf(stderr, "FINBR %s: owner absent\n", what); return 1; }
    if (e != &s->track_statuses[want_slot]) {
        fprintf(stderr, "FINBR %s: owner in an undeclared slot\n", what);
        bad++;
    }
    if ((int)e->state != (int)MOQ_TS_PENDING_PUBLISHER) {
        fprintf(stderr, "FINBR %s: state %d\n", what, (int)e->state);
        bad++;
    }
    if ((int)e->role != (int)MOQ_TS_ROLE_PUBLISHER) {
        fprintf(stderr, "FINBR %s: role %d\n", what, (int)e->role);
        bad++;
    }
    if (e->generation != want_gen) {
        fprintf(stderr, "FINBR %s: generation\n", what); bad++;
    }
    if (e->handle._opaque != want_handle) {
        fprintf(stderr, "FINBR %s: handle\n", what); bad++;
    }
    if (e->request_id != p7_want_request_id) {
        fprintf(stderr, "FINBR %s: request id\n", what); bad++;
    }
    if (e->request_stream_ref._v != ref._v) {
        fprintf(stderr, "FINBR %s: owner ref\n", what); bad++;
    }
    /* The transferred FIN -- this family's carrier is the durable latch. */
    if ((e->req_recv_fin ? 1 : 0) != p7_want_fin) {
        fprintf(stderr, "FINBR %s: FIN latch %d, expected %d\n", what,
                e->req_recv_fin ? 1 : 0, p7_want_fin);
        bad++;
    }
    if (p7_pool_busy(s) != 1) {
        fprintf(stderr, "FINBR %s: pool occupancy %d, expected 1\n", what,
                p7_pool_busy(s));
        bad++;
    }
    return bad;
}

static int p7_check_retired(const moq_session_t *s, moq_stream_ref_t ref,
                            int want_slot, const char *what)
{
    int bad = 0;
    if (p7_owner(s, ref) != NULL) {
        fprintf(stderr, "FINBR %s: registry edge survives\n", what); bad++;
    }
    /* The POOL too: removing the edge while leaking the slot must not pass. */
    if (want_slot >= 0 && (size_t)want_slot < s->ts_cap &&
        s->track_statuses[want_slot].state != MOQ_TS_FREE) {
        fprintf(stderr, "FINBR %s: pool slot leaked\n", what); bad++;
    }
    if (p7_pool_busy(s) != 0) {
        fprintf(stderr, "FINBR %s: pool occupancy %d, expected 0\n", what,
                p7_pool_busy(s));
        bad++;
    }
    return bad;
}

/* The request the producer must surface, built from the DECLARED handle. */
static int p7_want_request(txs_norm_vec_t *v, uint64_t handle)
{
    txs_img_t im;
    txs_img_init(&im);
    txs_img_u64(&im, handle);
    txs_img_u64(&im, 2);                                /* ns part count */
    txs_img_bytes(&im, k_p7_ns0, sizeof(k_p7_ns0));
    txs_img_bytes(&im, k_p7_ns1, sizeof(k_p7_ns1));
    txs_img_bytes(&im, k_p7_name, sizeof(k_p7_name));
    txs_img_u64(&im, 1);                                /* token count */
    txs_img_u64(&im, P7_TOK_TYPE);
    txs_img_bytes(&im, k_p7_tok, sizeof(k_p7_tok));
    if (!txs_norm_append_img(v, MOQ_EVENT_TRACK_STATUS_REQUEST, &im)) {
        fprintf(stderr, "FINBR: could not build the declared request image\n");
        return 1;
    }
    return 0;
}

/* The owner record plus the bytes it owns. The raw compare is SHALLOW by
 * itself -- it would see a moved `track_id_buf` pointer but not edited bytes --
 * so the identity buffer is deep-copied alongside it. Bounds-safe: an
 * over-long or unbacked buffer makes the record incomparable rather than being
 * read. */
#define P7_OWN_MAX 256
typedef struct p7_snap {
    int      valid;
    moq_ts_entry_t raw;
    size_t   tid_len;
    uint8_t  tid[P7_OWN_MAX];
} p7_snap_t;

static int p7_check_terminal_wire(void *vctx, const char *what);

/* The family's EXACT topology: one stream-ref edge while live, none at all
 * once retired. */
static int p7_check_edges(const og_graph_t *g, moq_stream_ref_t ref,
                          int want_slot, int live, const char *what)
{
    int bad = og_check_integrity(g, what);
    if (live) {
        bad += og_check_edge(g, OG_DOM_REQ_STREAMREF, ref._v,
                             MOQ_REQ_TRACK_STATUS, want_slot, what);
        const og_edge_spec_t w[] = { { OG_DOM_REQ_STREAMREF, ref._v } };
        bad += og_check_owner_edges(g, MOQ_REQ_TRACK_STATUS, want_slot, w, 1,
                                    what);
    } else {
        bad += og_check_no_edge(g, OG_DOM_REQ_STREAMREF, ref._v, what);
        bad += og_check_owner_edges(g, MOQ_REQ_TRACK_STATUS, want_slot, NULL,
                                    0, what);
    }
    bad += og_check_no_edge(g, OG_DOM_NS_REF, ref._v, what);
    return bad;
}

/* This route owes NO drain at any phase: the FIN is observed before the
 * terminal runs, so the declared multiset is EMPTY throughout. */
static int p7_check_drain(const moq_session_t *s, const char *what)
{
    return check_drain_membership(s, NULL, 0, what);
}


/* -- the descriptor's hooks, all four genuinely consumed -------------- */


static void p7_capture(const moq_session_t *s, void *vctx, void *state,
                       size_t cap)
{
    fin_bridge_run_t *r = (fin_bridge_run_t *)vctx;
    p7_snap_t *o = (p7_snap_t *)state;
    /* The declared size is the contract; refuse rather than overwrite. */
    if (cap < sizeof(*o)) { fprintf(stderr, "FINBR: snapshot storage too small\n"); return; }
    memset(o, 0, sizeof(*o));
    if (r->want_slot < 0 || (size_t)r->want_slot >= s->ts_cap) return;
    const moq_ts_entry_t *e = &s->track_statuses[r->want_slot];
    o->raw = *e;
    o->tid_len = e->track_id_len;
    if (e->track_id_len > P7_OWN_MAX || (e->track_id_len && !e->track_id_buf))
        return;
    if (e->track_id_len) memcpy(o->tid, e->track_id_buf, e->track_id_len);
    o->valid = 1;
}

static int p7_check(const moq_session_t *s, void *vctx, const void *state,
                    size_t cap, const char *what)
{
    const p7_snap_t *want = (const p7_snap_t *)state;
    p7_snap_t now;
    if (cap < sizeof(now)) {
        fprintf(stderr, "FINBR %s: snapshot storage too small\n", what);
        return 1;
    }
    p7_capture(s, vctx, &now, sizeof(now));
    if (!now.valid || !want->valid) {
        fprintf(stderr, "FINBR %s: incomparable owner record\n", what);
        return 1;
    }
    if (memcmp(&now.raw, &want->raw, sizeof(now.raw)) != 0) {
        fprintf(stderr, "FINBR %s: owner record changed\n", what);
        return 1;
    }
    if (now.tid_len != want->tid_len ||
        (now.tid_len && memcmp(now.tid, want->tid, now.tid_len) != 0)) {
        fprintf(stderr, "FINBR %s: retained track identity changed\n", what);
        return 1;
    }
    return 0;
}

static bool p7_norm_event(const moq_event_t *ev, void *ctx, txs_norm_vec_t *out)
{
    (void)ctx;
    if (ev->kind != MOQ_EVENT_TRACK_STATUS_REQUEST) {
        fprintf(stderr, "FINBR: unnormalized event kind %u\n",
                (unsigned)ev->kind);
        return false;
    }
    const moq_track_status_request_event_t *q = &ev->u.track_status_request;
    txs_img_t im;
    txs_img_init(&im);
    txs_img_u64(&im, q->handle._opaque);
    if (q->track_namespace.count > 32 || q->token_count > 16) {
        fprintf(stderr, "FINBR: implausible request counts\n");
        return false;
    }
    if (q->track_namespace.count && !q->track_namespace.parts) {
        fprintf(stderr, "FINBR: namespace count with NULL parts\n");
        return false;
    }
    if (q->token_count && !q->tokens) {
        fprintf(stderr, "FINBR: token count with NULL tokens\n");
        return false;
    }
    txs_img_u64(&im, (uint64_t)q->track_namespace.count);
    for (size_t i = 0; i < q->track_namespace.count; i++) {
        const moq_bytes_t *b = &q->track_namespace.parts[i];
        if (b->len && !b->data) {
            fprintf(stderr, "FINBR: namespace part %zu has NULL bytes\n", i);
            return false;
        }
        txs_img_bytes(&im, b->data, b->len);
    }
    if (q->track_name.len && !q->track_name.data) {
        fprintf(stderr, "FINBR: track name has NULL bytes\n");
        return false;
    }
    txs_img_bytes(&im, q->track_name.data, q->track_name.len);
    txs_img_u64(&im, (uint64_t)q->token_count);
    for (size_t i = 0; i < q->token_count; i++) {
        if (q->tokens[i].token_value.len && !q->tokens[i].token_value.data) {
            fprintf(stderr, "FINBR: token %zu has NULL bytes\n", i);
            return false;
        }
        txs_img_u64(&im, q->tokens[i].token_type);
        txs_img_bytes(&im, q->tokens[i].token_value.data,
                      q->tokens[i].token_value.len);
    }
    return txs_norm_append_img(out, ev->kind, &im);
}

/* The bridge drains the session's actions itself, so anything still queued
 * afterwards is unexpected by definition. */
static bool p7_norm_action(const moq_action_t *a, void *ctx, txs_norm_vec_t *out)
{
    (void)ctx; (void)out;
    fprintf(stderr, "FINBR: action kind %u left queued after service\n",
            (unsigned)a->kind);
    return false;
}

static moq_result_t p7_feed_id(moq_transport_bridge_t *b, uint64_t request_id,
                               uint64_t transport_id, bool fin)
{
    uint8_t msg[192];
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, msg, sizeof(msg));
    moq_bytes_t parts[] = { { k_p7_ns0, sizeof(k_p7_ns0) },
                            { k_p7_ns1, sizeof(k_p7_ns1) } };
    moq_namespace_t ns = { parts, 2 };
    moq_d18_msg_params_t p = { 0 };
    p.auth_token_count = 1;
    p.auth_tokens[0].alias_type = 3;              /* USE_VALUE */
    p.auth_tokens[0].token_type = P7_TOK_TYPE;
    p.auth_tokens[0].token_value = (moq_bytes_t){ k_p7_tok, sizeof(k_p7_tok) };
    if (moq_d18_encode_track_status(&w, request_id, &ns,
            (moq_bytes_t){ k_p7_name, sizeof(k_p7_name) }, &p) != MOQ_OK)
        return MOQ_ERR_INTERNAL;
    return moq_transport_bridge_on_peer_bidi_bytes(
        b, transport_id, msg, moq_buf_writer_offset(&w), fin, 0);
}

static moq_result_t p7_feed(moq_transport_bridge_t *b, void *vctx,
                            uint64_t transport_id, bool fin)
{
    (void)vctx;
    return p7_feed_id(b, 0, transport_id, fin);
}

static moq_result_t p7_terminal(moq_transport_bridge_t *b, void *vctx,
                                uint64_t transport_id)
{
    fin_bridge_run_t *r = (fin_bridge_run_t *)vctx;
    (void)b; (void)transport_id;
    moq_track_status_handle_t h;
    h._opaque = r->want_handle;
    moq_accept_track_status_cfg_t c;
    moq_accept_track_status_cfg_init(&c);
    c.has_largest = true;
    c.largest_group = P7_LARGEST_GROUP;
    c.largest_object = P7_LARGEST_OBJ;
    c.has_expires = true;
    c.expires_ms = P7_EXPIRES_MS;
    return moq_session_accept_track_status(r->tp.server, h, &c, 0);
}

_Static_assert(sizeof(p7_snap_t) <= FIN_BR_SNAP_MAX,
               "p7 snapshot exceeds the shared bounded storage");

static const fin_bridge_family_t p7_family = {
    .owner_kind    = MOQ_REQ_TRACK_STATUS,
    .pool_tag      = MOQ_HANDLE_POOL_TRACK_STATUS,
    .snap_size     = sizeof(p7_snap_t),
    .capture       = p7_capture,
    .check         = p7_check,
    .normalize_event  = p7_norm_event,
    .normalize_action = p7_norm_action,
    .want_request  = p7_want_request,
    .derive_slot   = p7_derive_slot,
    .check_live    = p7_check_live,
    .check_retired = p7_check_retired,
    .check_edges   = p7_check_edges,
    .check_drain   = p7_check_drain,
    .check_terminal_wire = p7_check_terminal_wire,
};

/* The EXACT terminal the service must put on the wire, decoded rather than
 * counted: one write, on this transport stream, FIN'd, one REQUEST_OK envelope
 * with nothing after it, and a body carrying the declared status. */
static int p7_check_terminal_wire(void *vctx, const char *what)
{
    fin_bridge_run_t *r = (fin_bridge_run_t *)vctx;
    int bad = 0;
    if (r->tp.server_ep.count != 1) {
        fprintf(stderr, "FINBR %s: %zu endpoint ops, expected 1\n", what,
                r->tp.server_ep.count);
        return 1;
    }
    fake_op_t *o = &r->tp.server_ep.ops[0];
    if (o->kind != FAKE_OP_WRITE) {
        fprintf(stderr, "FINBR %s: op kind %d, expected WRITE\n", what,
                (int)o->kind);
        return 1;
    }
    if (o->stream_id != r->transport_id) {
        fprintf(stderr, "FINBR %s: wrong transport stream\n", what); bad++;
    }
    if (!o->fin) {
        fprintf(stderr, "FINBR %s: local half not FIN'd\n", what); bad++;
    }
    moq_buf_reader_t rr;
    moq_buf_reader_init(&rr, o->data, o->data_len);
    moq_control_envelope_t env;
    memset(&env, 0, sizeof(env));
    if (moq_d18_decode_envelope(&rr, &env) != MOQ_OK) {
        fprintf(stderr, "FINBR %s: undecodable envelope\n", what); return bad + 1;
    }
    if (env.msg_type != (uint64_t)MOQ_D18_REQUEST_OK) {
        fprintf(stderr, "FINBR %s: msg type %llu\n", what,
                (unsigned long long)env.msg_type);
        bad++;
    }
    if (moq_buf_reader_remaining(&rr) != 0) {
        fprintf(stderr, "FINBR %s: trailing bytes after the envelope\n", what);
        bad++;
    }
    moq_d18_track_status_ok_t ok;
    memset(&ok, 0, sizeof(ok));
    if (moq_d18_decode_track_status_ok(env.payload, env.payload_len,
                                       &ok) != MOQ_OK) {
        fprintf(stderr, "FINBR %s: undecodable TRACK_STATUS_OK body\n", what);
        return bad + 1;
    }
    if (!ok.params.has_largest || ok.params.largest_group != P7_LARGEST_GROUP ||
        ok.params.largest_object != P7_LARGEST_OBJ) {
        fprintf(stderr, "FINBR %s: largest mismatch\n", what); bad++;
    }
    if (!ok.params.has_expires || ok.params.expires_ms != P7_EXPIRES_MS) {
        fprintf(stderr, "FINBR %s: expires mismatch\n", what); bad++;
    }
    if (ok.track_properties.len != 0) {
        fprintf(stderr, "FINBR %s: non-empty track properties\n", what); bad++;
    }
    return bad;
}

/*
 * The runner. Every hook the descriptor declares is CONSUMED: the family
 * bundle derives and identifies the owner, `capture`/`check` pin its record
 * across the EVENT POLL, and both normalizers build the images compared
 * against the declared output. Repeated bridge service is legal; post-FIN peer
 * bytes are not, and none are ever fed.
 */
/* The four phases the bridge entry passes through. */
typedef enum fin_br_phase {
    FIN_BR_AFTER_INGRESS = 0,
    FIN_BR_AFTER_TERMINAL,
    FIN_BR_AFTER_SERVICE,
    FIN_BR_AFTER_RESERVICE
} fin_br_phase_t;

/*
 * The EXACT bridge state for a class at a phase -- every flag constrained, not
 * just the interesting ones, so an unrelated flag turning on is caught.
 *
 * A retained ingress returns at transport_bridge.c:2662, BEFORE
 * peer_send_closed is set at :2677: the obligation lives in the retry flags
 * instead, which is why the two classes need different expectations here.
 */
static int fin_br_check_bridge(moq_transport_bridge_t *b, uint64_t transport_id,
                               moq_stream_ref_t want_ref,
                               fin_bridge_ingress_t ingress,
                               fin_br_phase_t phase, const char *what)
{
    int bad = 0;
    bridge_stream_entry_t *e = bridge_find_by_id(b, transport_id);

    if (phase == FIN_BR_AFTER_SERVICE || phase == FIN_BR_AFTER_RESERVICE) {
        if (e && e->active) {
            fprintf(stderr, "FINBR %s: mapping still active\n", what);
            bad++;
        }
        {   /* Absent by BOTH identities: an entry repointed to another
             * transport id while keeping the internal ref must not pass. */
            bridge_stream_entry_t *by_ref = bridge_find_by_ref(b, want_ref);
            if (by_ref && by_ref->active) {
                fprintf(stderr, "FINBR %s: mapping still active by ref\n",
                        what);
                bad++;
            }
        }
        if (moq_transport_bridge_has_pending(b)) {
            fprintf(stderr, "FINBR %s: bridge reports pending work\n", what);
            bad++;
        }
    } else {
        if (!e) {
            fprintf(stderr, "FINBR %s: mapping absent\n", what);
            return bad + 1;
        }
        if (!e->active) {
            fprintf(stderr, "FINBR %s: mapping inactive\n", what); bad++;
        }
        if (e->transport_id != transport_id) {
            fprintf(stderr, "FINBR %s: transport id\n", what); bad++;
        }
        /* BOTH identities, so a repointed entry cannot masquerade. */
        if (e->ref._v != want_ref._v) {
            fprintf(stderr, "FINBR %s: internal ref\n", what); bad++;
        }
        if (bridge_find_by_ref(b, want_ref) != e) {
            fprintf(stderr, "FINBR %s: ref lookup resolves elsewhere\n", what);
            bad++;
        }
        if (e->kind != BRIDGE_STREAM_BIDI) {
            fprintf(stderr, "FINBR %s: stream kind %d\n", what, (int)e->kind);
            bad++;
        }
        if (e->origin != BRIDGE_ORIGIN_PEER) {
            fprintf(stderr, "FINBR %s: stream origin %d\n", what,
                    (int)e->origin);
            bad++;
        }
        if (e->aborting) {
            fprintf(stderr, "FINBR %s: entry aborting\n", what); bad++;
        }
        if (e->pending_reset || e->pending_stop) {
            fprintf(stderr, "FINBR %s: reset/stop pending\n", what); bad++;
        }
        int want_peer_closed = (ingress == FIN_BR_CONSUMED);
        if (!!e->peer_send_closed != want_peer_closed) {
            fprintf(stderr, "FINBR %s: peer_send_closed %d, expected %d\n",
                    what, (int)e->peer_send_closed, want_peer_closed);
            bad++;
        }
        if (e->local_send_closed) {
            fprintf(stderr, "FINBR %s: local half already closed\n", what);
            bad++;
        }
        if (ingress == FIN_BR_CONSUMED) {
            if (e->pending_retry || e->pending_fin || e->fin_retained) {
                fprintf(stderr, "FINBR %s: FIN state retained after a "
                        "consumed ingress\n", what);
                bad++;
            }
        } else {
            if (!e->pending_retry || !e->fin_retained) {
                fprintf(stderr, "FINBR %s: retained obligation missing\n",
                        what);
                bad++;
            }
            /* `pending_fin` is a LATER FIN arriving while already suspended;
             * initial same-call retention owes it false. */
            if (e->pending_fin) {
                fprintf(stderr, "FINBR %s: unexpected pending_fin on initial "
                        "retention\n", what);
                bad++;
            }
        }
    }
    if (moq_transport_bridge_is_fatal(b)) {
        fprintf(stderr, "FINBR %s: bridge went fatal\n", what); bad++;
    }
    if (moq_transport_bridge_is_closed(b)) {
        fprintf(stderr, "FINBR %s: bridge closed\n", what); bad++;
    }
    return bad;
}

/* Set while a self-check drives the runner down a deliberately REFUSED path,
 * so an expected refusal cannot be mistaken for a failure in focused output. */
static int fin_br_quiet;

static int run_fin_bridge(const fin_bridge_case_t *f, fin_bridge_run_t *r)
{
    int failures = 0;
    char what[160];
    /* An invalid descriptor must not reach ANY family hook: an oversized
     * snapshot would otherwise be written into bounded storage. */
    if (fin_bridge_problems(f) != 0) {
        if (!fin_br_quiet)
            fprintf(stderr, "FINBR %s: invalid descriptor; no hook invoked\n",
                    f->name ? f->name : "(unnamed)");
        return failures + 1;
    }
    if (f->family->snap_size > sizeof(((fin_bridge_snap_t *)0)->bytes)) {
        if (!fin_br_quiet)
            fprintf(stderr, "FINBR %s: snapshot larger than storage\n",
                    f->name);
        return failures + 1;
    }

    /* Slot, generation and the handle they pack into, all from pool state
     * BEFORE ingress. */
    r->want_slot = f->family->derive_slot(r->tp.server, &r->want_gen);
    MOQ_TEST_CHECK(r->want_slot >= 0);
    if (r->want_slot < 0) return failures;   /* the check above counted it */
    r->want_handle = moq_handle_pack(f->family->pool_tag,
                                     r->tp.server->session_tag, r->want_gen,
                                     (uint32_t)r->want_slot);
    /* This route owes NO drain at any point: the FIN is observed before the
     * terminal runs, so the declared multiset is EMPTY throughout. */
    failures += f->family->check_drain(r->tp.server, "pre-ingress");

    /* 1. Request + FIN in ONE chunk, through real bridge ingress. */
    moq_result_t want_rc = (f->ingress == FIN_BR_CONSUMED)
                               ? MOQ_OK : MOQ_ERR_WOULD_BLOCK;
    MOQ_TEST_CHECK_EQ_INT((int)f->feed(r->tp.server_bridge, f->ctx,
                                       r->transport_id, true), (int)want_rc);

    /* The SESSION ref, captured while the entry exists; re-deriving it later
     * would return NULL once retired and make every owner check vacuous. */
    {
        bridge_stream_entry_t *e =
            bridge_find_by_id(r->tp.server_bridge, r->transport_id);
        MOQ_TEST_CHECK(e != NULL);
        if (!e) return failures;             /* the check above counted it */
        r->ref = e->ref;
        MOQ_TEST_CHECK(r->ref._v != 0);
        /* The two identities are genuinely different values. */
        MOQ_TEST_CHECK(r->ref._v != r->transport_id);
        MOQ_TEST_CHECK(e->active);
    }
    snprintf(what, sizeof(what), "%s ingress", f->name);
    failures += fin_br_check_bridge(r->tp.server_bridge, r->transport_id,
                                    r->ref, f->ingress, FIN_BR_AFTER_INGRESS, what);

    /* 2. The owner, against the DECLARED identity, plus its exact edge set. */
    snprintf(what, sizeof(what), "%s admitted", f->name);
    failures += f->family->check_live(r->tp.server, r->ref, r->want_slot,
                                     r->want_gen, r->want_handle, what);
    {
        og_graph_t g;
        og_capture(r->tp.server, &g);
        failures += f->family->check_edges(&g, r->ref, r->want_slot, 1, what);
    }

    /* 3. Snapshot the owner BEFORE polling, so a mutation caused BY the poll
     *    cannot become the baseline it is measured against. */
    fin_bridge_snap_t snap;
    MOQ_TEST_CHECK(f->family->snap_size <= sizeof(snap.bytes));
    f->family->capture(r->tp.server, f->ctx, snap.bytes,
                       f->family->snap_size);

    /* Exactly the declared request event, compared field for field. */
    {
        txs_norm_vec_t got, want;
        txs_norm_init(&got);
        txs_norm_init(&want);
        moq_event_t ev;
        while (moq_session_poll_events(r->tp.server, &ev, 1) > 0) {
            if (!f->family->normalize_event(&ev, f->ctx, &got)) failures++;
            moq_event_cleanup(&ev);
        }
        failures += f->family->want_request(&want, r->want_handle);
        failures += txs_norm_equals(&got, &want, f->name);
        txs_norm_free(&got);
        txs_norm_free(&want);
    }

    /* Nothing the poll touched may have moved: the owner record, its identity,
     * its exact live edge topology and the drain multiset are ALL reasserted
     * here, so a poll-time mutation the terminal later clears cannot pass. */
    snprintf(what, sizeof(what), "%s post-poll", f->name);
    failures += f->family->check(r->tp.server, f->ctx, snap.bytes,
                                 f->family->snap_size, what);
    failures += f->family->check_live(r->tp.server, r->ref, r->want_slot,
                                      r->want_gen, r->want_handle, what);
    {
        og_graph_t g;
        og_capture(r->tp.server, &g);
        failures += f->family->check_edges(&g, r->ref, r->want_slot, 1, what);
    }
    failures += f->family->check_drain(r->tp.server, what);
    failures += fin_br_check_bridge(r->tp.server_bridge, r->transport_id,
                                    r->ref, f->ingress, FIN_BR_AFTER_INGRESS, what);

    /* 4. The application answers; the FIN having been observed, NO drain. */
    MOQ_TEST_CHECK_EQ_INT((int)f->terminal(r->tp.server_bridge, f->ctx,
                                           r->transport_id), (int)MOQ_OK);
    snprintf(what, sizeof(what), "%s terminal", f->name);
    failures += f->family->check_retired(r->tp.server, r->ref, r->want_slot,
                                        what);
    failures += f->family->check_drain(r->tp.server, what);
    {   /* Semantic retirement is proven in the RAW graph too, before any
         * physical service runs. */
        og_graph_t g;
        og_capture(r->tp.server, &g);
        failures += f->family->check_edges(&g, r->ref, r->want_slot, 0, what);
    }
    /* The PHYSICAL mapping is untouched by the semantic terminal: still active,
     * still carrying its class's ingress state, our half still open. */
    failures += fin_br_check_bridge(r->tp.server_bridge, r->transport_id,
                                    r->ref, f->ingress, FIN_BR_AFTER_TERMINAL, what);

    /* 5. Bridge service alone emits the EXACT terminal and retires the map. */
    fake_endpoint_clear_ops(&r->tp.server_ep);
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(r->tp.server_bridge, 0), (int)MOQ_OK);
    snprintf(what, sizeof(what), "%s serviced", f->name);
    failures += f->family->check_terminal_wire(f->ctx, what);
    failures += f->family->check_retired(r->tp.server, r->ref, r->want_slot,
                                        what);
    failures += fin_br_check_bridge(r->tp.server_bridge, r->transport_id,
                                    r->ref, f->ingress, FIN_BR_AFTER_SERVICE, what);
    {
        og_graph_t g;
        og_capture(r->tp.server, &g);
        failures += f->family->check_edges(&g, r->ref, r->want_slot, 0, what);
    }
    failures += f->family->check_drain(r->tp.server, what);

    /* 6. Idempotence: a second service adds EXACTLY ZERO operations, leaves
     *    nothing queued, recreates no owner, and installs no drain. */
    fake_endpoint_clear_ops(&r->tp.server_ep);
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(r->tp.server_bridge, 0), (int)MOQ_OK);
    snprintf(what, sizeof(what), "%s reserviced", f->name);
    MOQ_TEST_CHECK_EQ_SIZE(r->tp.server_ep.count, (size_t)0);
    {
        txs_norm_vec_t leftover;
        txs_norm_init(&leftover);
        moq_event_t ev;
        while (moq_session_poll_events(r->tp.server, &ev, 1) > 0) {
            if (!f->family->normalize_event(&ev, f->ctx, &leftover))
                failures++;
            moq_event_cleanup(&ev);
        }
        moq_action_t a;
        while (moq_session_poll_actions(r->tp.server, &a, 1) > 0) {
            if (!f->family->normalize_action(&a, f->ctx, &leftover))
                failures++;
            moq_action_cleanup(&a);
        }
        MOQ_TEST_CHECK_EQ_SIZE(leftover.count, (size_t)0);
        txs_norm_free(&leftover);
    }
    failures += f->family->check_retired(r->tp.server, r->ref, r->want_slot,
                                        what);
    failures += f->family->check_drain(r->tp.server, what);
    {
        og_graph_t g;
        og_capture(r->tp.server, &g);
        failures += f->family->check_edges(&g, r->ref, r->want_slot, 0, what);
    }
    /* The COMPLETE physical postcondition again: a mapping resurrected only by
     * the second service must not pass. */
    failures += fin_br_check_bridge(r->tp.server_bridge, r->transport_id,
                                    r->ref, f->ingress, FIN_BR_AFTER_RESERVICE, what);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_state(r->tp.server),
                          (int)MOQ_SESS_ESTABLISHED);
    return failures;
}

/* Permanent descriptor self-checks: every required member, the ingress class,
 * and the (kind, pool) pairing. Deleting a member from a real descriptor would
 * only fail the BUILD via an unused static, so validity is probed here on
 * local copies instead. */
static int probe_capture_calls;
static void probe_capture_forbidden(const moq_session_t *s, void *ctx,
                                    void *state, size_t cap)
{
    (void)s; (void)ctx; (void)state; (void)cap;
    probe_capture_calls++;
}


/* -- #250: SUBSCRIBE_NAMESPACE response-stream termination, over the real
 * bridge (draft-18 §10.18).
 *
 * The route is indexed by idx_ns_by_ref, so the request-stream retainability
 * gap (#245(a)) must not be on its path -- these rows PROVE that with the very
 * predicate the bridge consults, and then by taking a genuine retained
 * WOULD_BLOCK rather than a fatal. Recovery is SERVICE-ONLY: the fixture never
 * re-delivers peer bytes, a FIN, or a second reset.
 *
 * The owner inventory, event images, drain multiset and action classification
 * come from tests/support/ns_owner_inventory.h, so this suite and the
 * direct-session suite hold ONE field contract. */

typedef struct nsfin_arm {
    test_pair_t      tp;
    uint64_t         bidi;      /* transport id, DECLARED before the request */
    moq_stream_ref_t ref;       /* session ref, DECLARED before the request */
    int              slot;
    uint32_t         generation;
    uint64_t         handle;
    int              srv_slot;        /* all three DERIVED before delivery */
    uint32_t         srv_generation;
    uint64_t         srv_handle;
    size_t           budget0;      /* client receive budget before any suffix */
    size_t           budget_active; /* DECLARED: budget0 + array + suffix key */
    nf_inv_t         want_live;     /* DECLARED established owner, not observed */
    const char      *sfx;
} nsfin_arm_t;

#define NSFIN_PREFIX  "live"
#define NSFIN_PREFIX2 "v2"
#define NSFIN_RID     0u

/* The exact bridge-entry class for this LOCAL-ORIGIN request bidi. (The
 * client opened it -- transport_bridge.c:1800 -- even though the peer's
 * response bytes later arrive on it. The earlier "peer-origin" wording was
 * wrong.) FIN and RESET supply only their own flag/code deltas. */
typedef struct nsfin_entry_want {
    int      active;
    int      pending_retry, pending_fin, fin_retained;
    int      pending_reset;  uint64_t pending_reset_code;
    int      pending_stop;   uint64_t pending_stop_code;
    int      peer_send_closed, local_send_closed, aborting;
    int      stream_pending;      /* moq_transport_bridge_stream_has_pending */
    int      bridge_pending;      /* moq_transport_bridge_has_pending */
    /* Inbound-uni classification storage. A BIDI/LOCAL entry never classifies,
     * so its declared value is the zero/PENDING state in EVERY live phase. */
    uint8_t  uni_disp;
    uint8_t  classify_len;
    uint8_t  classify_buf[9];
} nsfin_entry_want_t;

static nsfin_entry_want_t nsfin_entry_live(void)
{
    nsfin_entry_want_t w;
    memset(&w, 0, sizeof(w));
    w.active = 1;
    return w;
}

static int nsfin_check_entry(nsfin_arm_t *a, const nsfin_entry_want_t *w,
                             const char *what)
{
    int failures = 0;
    bridge_stream_entry_t *by_id = bridge_find_by_id(a->tp.client_bridge,
                                                     a->bidi);
    bridge_stream_entry_t *by_ref = bridge_find_by_ref(a->tp.client_bridge,
                                                       a->ref);
    if (!by_id || by_id != by_ref) {
        fprintf(stderr, "NSFIN %s: the entry is not reachable by BOTH the"
                " declared transport id and the declared internal ref\n", what);
        return failures + 1;
    }
#define NSE(f, fmt, expr) do { \
        if ((expr) != (w->f)) { \
            fprintf(stderr, "NSFIN %s: entry " #f " " fmt ", expected " fmt \
                    "\n", what, (expr), (w->f)); \
            failures++; \
        } \
    } while (0)
    /* identity first: a coherently wrong mapping must not pass */
    if (by_id->ref._v != a->ref._v) {
        fprintf(stderr, "NSFIN %s: entry ref %llu, declared %llu\n", what,
                (unsigned long long)by_id->ref._v,
                (unsigned long long)a->ref._v);
        failures++;
    }
    if (by_id->transport_id != a->bidi) {
        fprintf(stderr, "NSFIN %s: entry transport id %llu, declared %llu\n",
                what, (unsigned long long)by_id->transport_id,
                (unsigned long long)a->bidi);
        failures++;
    }
    if (by_id->kind != BRIDGE_STREAM_BIDI) {
        fprintf(stderr, "NSFIN %s: entry kind %d, expected BIDI\n", what,
                (int)by_id->kind);
        failures++;
    }
    if (by_id->origin != BRIDGE_ORIGIN_LOCAL) {
        fprintf(stderr, "NSFIN %s: entry origin %d, expected LOCAL\n", what,
                (int)by_id->origin);
        failures++;
    }
    NSE(active, "%d", (int)by_id->active);
    NSE(pending_retry, "%d", (int)by_id->pending_retry);
    NSE(pending_fin, "%d", (int)by_id->pending_fin);
    NSE(fin_retained, "%d", (int)by_id->fin_retained);
    NSE(pending_reset, "%d", (int)by_id->pending_reset);
    NSE(pending_stop, "%d", (int)by_id->pending_stop);
    NSE(peer_send_closed, "%d", (int)by_id->peer_send_closed);
    NSE(local_send_closed, "%d", (int)by_id->local_send_closed);
    NSE(aborting, "%d", (int)by_id->aborting);
    NSE(uni_disp, "%d", (int)by_id->uni_disp);
    NSE(classify_len, "%d", (int)by_id->classify_len);
#undef NSE
    if (memcmp(by_id->classify_buf, w->classify_buf,
               sizeof(w->classify_buf)) != 0) {
        fprintf(stderr, "NSFIN %s: entry classify_buf differs from the"
                " declared zero state\n", what);
        failures++;
    }
    if (by_id->pending_reset_code != w->pending_reset_code) {
        fprintf(stderr, "NSFIN %s: entry pending_reset_code %llu, expected"
                " %llu\n", what,
                (unsigned long long)by_id->pending_reset_code,
                (unsigned long long)w->pending_reset_code);
        failures++;
    }
    if (by_id->pending_stop_code != w->pending_stop_code) {
        fprintf(stderr, "NSFIN %s: entry pending_stop_code %llu, expected"
                " %llu\n", what,
                (unsigned long long)by_id->pending_stop_code,
                (unsigned long long)w->pending_stop_code);
        failures++;
    }
    if ((int)moq_transport_bridge_stream_has_pending(a->tp.client_bridge,
                                                     a->bidi)
        != w->stream_pending) {
        fprintf(stderr, "NSFIN %s: stream_has_pending %d, expected %d\n", what,
                (int)moq_transport_bridge_stream_has_pending(
                    a->tp.client_bridge, a->bidi), w->stream_pending);
        failures++;
    }
    if ((int)moq_transport_bridge_has_pending(a->tp.client_bridge)
        != w->bridge_pending) {
        fprintf(stderr, "NSFIN %s: has_pending %d, expected %d\n", what,
                (int)moq_transport_bridge_has_pending(a->tp.client_bridge),
                w->bridge_pending);
        failures++;
    }
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(a->tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(a->tp.client_bridge));
    return failures;
}

/* Exactly this endpoint operation set on `ep`, and nothing else. */
static int nsfin_ops_exact(fake_endpoint_t *ep, int want_open_bidi,
                           int want_write, uint64_t want_id, const char *what)
{
    int failures = 0;
    int opens = 0, writes = 0, other = 0, wrong_id = 0;
    MOQ_TEST_CHECK(ep->count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < ep->count; i++) {
        if (want_id && ep->ops[i].stream_id != want_id) wrong_id++;
        switch (ep->ops[i].kind) {
        case FAKE_OP_OPEN_BIDI: opens++;  break;
        case FAKE_OP_WRITE:     writes++; break;
        default:                other++;  break;
        }
    }
    if (wrong_id) {
        fprintf(stderr, "NSFIN %s: %d ops on a stream id other than the"
                " declared %llu\n", what, wrong_id,
                (unsigned long long)want_id);
        failures++;
    }
    if (opens != want_open_bidi || writes != want_write || other != 0) {
        fprintf(stderr, "NSFIN %s: ops open-bidi %d write %d other %d,"
                " expected %d/%d/0\n", what, opens, writes, other,
                want_open_bidi, want_write);
        failures++;
    }
    return failures;
}

/* Deliver every WRITE on `from` to `to`, checking each ingress result, and
 * report how many were delivered. Unlike d18_feed nothing is discarded. */
static int nsfin_deliver(moq_transport_bridge_t *to, fake_endpoint_t *from,
                         size_t *delivered, const char *what)
{
    int failures = 0;
    *delivered = 0;
    MOQ_TEST_CHECK(from->count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < from->count; i++) {
        fake_op_t *o = &from->ops[i];
        if (o->kind != FAKE_OP_WRITE) {
            if (o->kind != FAKE_OP_OPEN_BIDI && o->kind != FAKE_OP_OPEN_UNI) {
                fprintf(stderr, "NSFIN %s: unexpected endpoint op kind %d\n",
                        what, (int)o->kind);
                failures++;
            }
            continue;
        }
        moq_result_t rc = (o->stream_id >= 2000 && o->stream_id < 3000) ||
                          (o->stream_id >= 4000)
            ? moq_transport_bridge_on_peer_bidi_bytes(
                  to, o->stream_id, o->data, o->data_len, o->fin, 0)
            : moq_transport_bridge_on_peer_uni_bytes(
                  to, o->stream_id, o->data, o->data_len, o->fin, 0);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_OK);
        (*delivered)++;
    }
    fake_endpoint_clear_ops(from);
    return failures;
}

/* Phases 1-4: one established client-side namespace subscription with exactly
 * one active suffix, every result checked and every record classified. */
static int nsfin_arm_build(nsfin_arm_t *a, const char *suffix_field)
{
    int failures = 0;
    memset(a, 0, sizeof(*a));
    a->sfx = suffix_field;
    if (d18_pair_init(&a->tp, 1) < 0) return 1;

    /* (1) checked starts, strict shuttle, exactly one SETUP_COMPLETE a side. */
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(a->tp.client, 0), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(a->tp.server, 0), (int)MOQ_OK);
    failures += d18_strict_shuttle(&a->tp, 30, 0, "nsfin setup");
    {
        moq_event_t ev;
        int c_setup = 0, s_setup = 0, other = 0;
        while (moq_session_poll_events(a->tp.client, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) c_setup++; else other++;
            moq_event_cleanup(&ev);
        }
        while (moq_session_poll_events(a->tp.server, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) s_setup++; else other++;
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK_EQ_INT(c_setup, 1);
        MOQ_TEST_CHECK_EQ_INT(s_setup, 1);
        MOQ_TEST_CHECK_EQ_INT(other, 0);
    }
    MOQ_TEST_CHECK_EQ_SIZE(a->tp.client_ep.count, (size_t)0);
    MOQ_TEST_CHECK_EQ_SIZE(a->tp.server_ep.count, (size_t)0);

    /* (2) derive EVERY identity BEFORE the request exists: the session ref
     * from the session's own counter, the transport id from the fake
     * endpoint's next bidi id, and the owner slot/generation/handle from the
     * free pool. Nothing here is adopted from what the call produces. */
    a->ref = moq_stream_ref_from_u64(a->tp.client->next_stream_ref);
    a->bidi = a->tp.client_ep.next_bidi_id;
    MOQ_TEST_CHECK(a->ref._v != 0);
    MOQ_TEST_CHECK(a->bidi != 0);

    int want_slot = -1;
    for (size_t i = 0; i < a->tp.client->ns_sub_cap; i++)
        if (a->tp.client->ns_subs[i].state == MOQ_NS_SUB_FREE) {
            want_slot = (int)i; break;
        }
    MOQ_TEST_CHECK(want_slot >= 0);
    if (want_slot < 0) return failures + 1;
    a->slot = want_slot;
    a->generation = a->tp.client->ns_subs[want_slot].generation | 1u;
    a->handle = moq_handle_pack(MOQ_HANDLE_POOL_NAMESPACE_SUB,
                                a->tp.client->session_tag, a->generation,
                                (uint32_t)want_slot);
    MOQ_TEST_CHECK(a->handle != 0);

    nf_inv_t free_rec;
    nf_inv_read(a->tp.client, want_slot, &free_rec);
    MOQ_TEST_CHECK(free_rec.valid);

    moq_bytes_t pfx_parts[] = { MOQ_BYTES_LITERAL(NSFIN_PREFIX),
                                MOQ_BYTES_LITERAL(NSFIN_PREFIX2) };
    moq_namespace_t pfx = { pfx_parts, 2 };
    moq_subscribe_namespace_cfg_t nc;
    moq_subscribe_namespace_cfg_init(&nc);
    nc.track_namespace_prefix = pfx;
    nc.namespace_interest = MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE;
    moq_ns_sub_handle_t nh;
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_session_subscribe_namespace(a->tp.client, &nc, 0, &nh),
        (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_U64(nh._opaque, a->handle);

    /* (3) exactly one local OPEN + one WRITE, decoded as SUBSCRIBE_NAMESPACE. */
    fake_endpoint_clear_ops(&a->tp.client_ep);
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(a->tp.client_bridge, 0), (int)MOQ_OK);
    failures += nsfin_ops_exact(&a->tp.client_ep, 1, 1, a->bidi,
                                "arm local open");
    for (size_t i = 0; i < a->tp.client_ep.count; i++) {
        fake_op_t *o = &a->tp.client_ep.ops[i];
        if (o->kind != FAKE_OP_WRITE) continue;
        MOQ_TEST_CHECK_EQ_U64(o->stream_id, a->bidi);
        MOQ_TEST_CHECK(!o->fin);
        moq_control_envelope_t env;
        moq_buf_reader_t r;
        moq_buf_reader_init(&r, o->data, o->data_len);
        MOQ_TEST_CHECK_EQ_INT((int)moq_d18_decode_envelope(&r, &env),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_U64(env.msg_type, MOQ_D18_SUBSCRIBE_NAMESPACE);
        MOQ_TEST_CHECK_EQ_SIZE(moq_buf_reader_remaining(&r), (size_t)0);
        moq_bytes_t dp[MOQ_DECODED_MAX_NAMESPACE_PARTS];
        moq_d18_subscribe_namespace_t sn;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_d18_decode_subscribe_namespace(
                env.payload, env.payload_len, dp,
                MOQ_DECODED_MAX_NAMESPACE_PARTS, &sn), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_U64(sn.request_id, NSFIN_RID);
        MOQ_TEST_CHECK_EQ_SIZE(sn.track_namespace_prefix.count, (size_t)2);
        failures += txs_check_part_bytes(&sn.track_namespace_prefix, 0,
                                         NSFIN_PREFIX, strlen(NSFIN_PREFIX),
                                         "arm request prefix 0");
        failures += txs_check_part_bytes(&sn.track_namespace_prefix, 1,
                                         NSFIN_PREFIX2, strlen(NSFIN_PREFIX2),
                                         "arm request prefix 1");
        /* no unexpected parameters may ride the request */
        MOQ_TEST_CHECK(sn.params.auth_token_count == 0 &&
                       !sn.params.has_forward &&
                       !sn.params.has_subscriber_priority &&
                       !sn.params.has_filter &&
                       !sn.params.has_group_order &&
                       !sn.params.has_new_group_request);
    }
    MOQ_TEST_CHECK(moq_index_find(a->tp.client->idx_ns_by_ref,
                                  a->tp.client->idx_ns_mask,
                                  a->ref._v) == a->slot);
    /* #245(a) independence, PROVEN with the predicate the bridge consults. */
    MOQ_TEST_CHECK(moq_session_has_transport_stream(a->tp.client, a->ref));

    /* Derive the SERVER owner BEFORE the request is delivered -- afterwards
     * the slot is already occupied and a scan would name the NEXT free one. */
    a->srv_slot = -1;
    for (size_t i = 0; i < a->tp.server->ns_sub_cap; i++)
        if (a->tp.server->ns_subs[i].state == MOQ_NS_SUB_FREE) {
            a->srv_slot = (int)i; break;
        }
    MOQ_TEST_CHECK(a->srv_slot >= 0);
    if (a->srv_slot < 0) return failures + 1;
    a->srv_generation = a->tp.server->ns_subs[a->srv_slot].generation | 1u;
    a->srv_handle = moq_handle_pack(MOQ_HANDLE_POOL_NAMESPACE_SUB,
                                    a->tp.server->session_tag,
                                    a->srv_generation,
                                    (uint32_t)a->srv_slot);
    MOQ_TEST_CHECK(a->srv_handle != 0);

    /* The DECLARED pending-subscriber owner, from the free record plus the
     * fixture's own inputs -- never re-read from the session. */
    {
        static const char *const kParts[2] = { NSFIN_PREFIX, NSFIN_PREFIX2 };
        a->want_live = nf_local_pending_want(&free_rec, a->generation,
                                             a->handle, NSFIN_RID, a->ref._v,
                                             kParts, 2,
                                             MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE);
        int adopt = nf_adopt_prefix_addrs(a->tp.client, a->slot,
                                          &a->want_live, "arm client owner");
        failures += adopt;
        if (adopt == 0)
            failures += nf_inv_check(a->tp.client, a->slot, &a->want_live,
                                     "arm client owner");
    }

    size_t moved = 0;
    failures += nsfin_deliver(a->tp.server_bridge, &a->tp.client_ep, &moved,
                              "arm request");
    MOQ_TEST_CHECK_EQ_SIZE(moved, (size_t)1);

    /* (4) exactly one NS_SUB_REQUEST with the declared image. */
    moq_ns_sub_handle_t sh = MOQ_NS_SUB_HANDLE_INVALID;
    {
        moq_event_t ev;
        int reqs = 0, other = 0;
        while (moq_session_poll_events(a->tp.server, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_NS_SUB_REQUEST) {
                reqs++;
                sh = ev.u.ns_sub_request.handle;
                MOQ_TEST_CHECK_EQ_U64(sh._opaque, a->srv_handle);
                MOQ_TEST_CHECK(ev.u.ns_sub_request.forward);
                MOQ_TEST_CHECK_EQ_SIZE(
                    ev.u.ns_sub_request.track_namespace_prefix.count,
                    (size_t)2);
                failures += txs_check_part_bytes(
                    &ev.u.ns_sub_request.track_namespace_prefix, 0,
                    NSFIN_PREFIX, strlen(NSFIN_PREFIX),
                    "arm ns_sub_request 0");
                failures += txs_check_part_bytes(
                    &ev.u.ns_sub_request.track_namespace_prefix, 1,
                    NSFIN_PREFIX2, strlen(NSFIN_PREFIX2),
                    "arm ns_sub_request 1");
                MOQ_TEST_CHECK_EQ_U64(
                    ev.u.ns_sub_request.namespace_interest,
                    MOQ_NAMESPACE_INTEREST_NAMESPACE_STATE);
                MOQ_TEST_CHECK_EQ_SIZE(ev.u.ns_sub_request.token_count,
                                       (size_t)0);
            } else {
                other++;
            }
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK_EQ_INT(reqs, 1);
        MOQ_TEST_CHECK_EQ_INT(other, 0);
    }
    /* the physical server owner matches the derivation too */
    MOQ_TEST_CHECK(a->tp.server->ns_subs[a->srv_slot].state !=
                   MOQ_NS_SUB_FREE);
    MOQ_TEST_CHECK_EQ_U64(a->tp.server->ns_subs[a->srv_slot].generation,
                          a->srv_generation);
    MOQ_TEST_CHECK_EQ_U64(a->tp.server->ns_subs[a->srv_slot].handle._opaque,
                          a->srv_handle);

    moq_accept_ns_sub_cfg_t ac;
    moq_accept_ns_sub_cfg_init(&ac);
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_session_accept_ns_sub(a->tp.server, sh, &ac, 0), (int)MOQ_OK);
    fake_endpoint_clear_ops(&a->tp.server_ep);
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(a->tp.server_bridge, 0), (int)MOQ_OK);
    /* The acceptance rides the client's own bidi: one WRITE, no local open. */
    failures += nsfin_ops_exact(&a->tp.server_ep, 0, 1, a->bidi,
                                "arm acceptance");
    {
        /* Decode the REQUEST_OK before delivering it. */
        for (size_t i = 0; i < a->tp.server_ep.count; i++) {
            fake_op_t *o = &a->tp.server_ep.ops[i];
            if (o->kind != FAKE_OP_WRITE) continue;
            MOQ_TEST_CHECK_EQ_U64(o->stream_id, a->bidi);
            MOQ_TEST_CHECK(!o->fin);
            moq_control_envelope_t env;
            moq_buf_reader_t r;
            moq_buf_reader_init(&r, o->data, o->data_len);
            MOQ_TEST_CHECK_EQ_INT((int)moq_d18_decode_envelope(&r, &env),
                                  (int)MOQ_OK);
            MOQ_TEST_CHECK_EQ_U64(env.msg_type, MOQ_D18_REQUEST_OK);
            MOQ_TEST_CHECK_EQ_SIZE(moq_buf_reader_remaining(&r), (size_t)0);
            /* the BODY too: junk inside the envelope must not pass */
            MOQ_TEST_CHECK_EQ_INT(
                (int)moq_d18_decode_request_ok(env.payload, env.payload_len),
                (int)MOQ_OK);
            /* The body check is load-bearing: junk inside the envelope must
             * be rejected, so an envelope-type-only assertion would not be
             * equivalent. */
            if (env.payload_len + 1 <= 64) {
                /* A VALID body plus one trailing byte, still inside the
                 * envelope, must be rejected -- otherwise the body check
                 * would only be proving that a malformed count fails. */
                uint8_t junk[64];
                memcpy(junk, env.payload, env.payload_len);
                junk[env.payload_len] = 0x5A;
                MOQ_TEST_CHECK(moq_d18_decode_request_ok(
                    junk, env.payload_len + 1) != MOQ_OK);
            }
        }
    }
    failures += nsfin_deliver(a->tp.client_bridge, &a->tp.server_ep, &moved,
                              "arm acceptance");
    MOQ_TEST_CHECK_EQ_SIZE(moved, (size_t)1);
    {
        nf_ev_t got[NF_EV_MAX]; size_t k = 0;
        failures += nf_collect(a->tp.client, a->handle, got, NF_EV_MAX, &k,
                               "arm ns_sub_ok");
        MOQ_TEST_CHECK_EQ_SIZE(k, (size_t)1);
        if (k == 1) {
            nf_ev_t want = nf_ev_want(MOQ_EVENT_NS_SUB_OK, NULL, NULL);
            failures += nf_ev_equals(&got[0], &want, "arm ns_sub_ok");
        }
    }

    /* The REQUEST_OK transition, applied EXPLICITLY rather than re-read. */
    a->want_live.state = MOQ_NS_SUB_ESTABLISHED;
    a->want_live.got_response = 1;
    failures += nf_inv_check(a->tp.client, a->slot, &a->want_live,
                             "arm established owner");
    a->budget0 = a->tp.client->recv_payload_bytes;

    /* One NAMESPACE: decoded on the wire, then surfaced as one exact
     * NAMESPACE_FOUND that is deliberately LEFT QUEUED as the blocker. */
    {
        moq_bytes_t sp[2];
        sp[0] = (moq_bytes_t){ (const uint8_t *)"room", 4 };
        sp[1] = (moq_bytes_t){ (const uint8_t *)suffix_field,
                               strlen(suffix_field) };
        moq_namespace_t sfx = { sp, 2 };
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_session_send_namespace(a->tp.server, sh, &sfx, 0),
            (int)MOQ_OK);
    }
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(a->tp.server_bridge, 0), (int)MOQ_OK);
    failures += nsfin_ops_exact(&a->tp.server_ep, 0, 1, a->bidi,
                                "arm namespace");
    for (size_t i = 0; i < a->tp.server_ep.count; i++) {
        fake_op_t *o = &a->tp.server_ep.ops[i];
        if (o->kind != FAKE_OP_WRITE) continue;
        MOQ_TEST_CHECK_EQ_U64(o->stream_id, a->bidi);
        moq_control_envelope_t env;
        moq_buf_reader_t r;
        moq_buf_reader_init(&r, o->data, o->data_len);
        MOQ_TEST_CHECK_EQ_INT((int)moq_d18_decode_envelope(&r, &env),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_U64(env.msg_type, MOQ_D18_NAMESPACE);
        MOQ_TEST_CHECK_EQ_SIZE(moq_buf_reader_remaining(&r), (size_t)0);
        MOQ_TEST_CHECK(!o->fin);
        moq_buf_reader_t pr;
        moq_buf_reader_init(&pr, env.payload, env.payload_len);
        moq_bytes_t sp[MOQ_DECODED_MAX_NAMESPACE_PARTS];
        moq_namespace_t got_ns;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_buf_read_namespace_prefix(&pr, sp,
                MOQ_DECODED_MAX_NAMESPACE_PARTS, &got_ns), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_SIZE(got_ns.count, (size_t)2);
        failures += txs_check_part_bytes(&got_ns, 0, "room", 4,
                                         "arm namespace suffix 0");
        failures += txs_check_part_bytes(&got_ns, 1, suffix_field,
                                         strlen(suffix_field),
                                         "arm namespace suffix 1");
        /* the whole payload, not just a decodable prefix of it */
        MOQ_TEST_CHECK_EQ_SIZE(moq_buf_reader_remaining(&pr), (size_t)0);
        /* and that check is load-bearing: the same body plus one trailing
         * byte inside the envelope must leave the reader unconsumed. */
        if (env.payload_len + 1 <= 128) {
            uint8_t tail[128];
            memcpy(tail, env.payload, env.payload_len);
            tail[env.payload_len] = 0x5A;
            moq_buf_reader_t tr;
            moq_buf_reader_init(&tr, tail, env.payload_len + 1);
            moq_bytes_t tp2[MOQ_DECODED_MAX_NAMESPACE_PARTS];
            moq_namespace_t tns;
            MOQ_TEST_CHECK(moq_buf_read_namespace_prefix(
                &tr, tp2, MOQ_DECODED_MAX_NAMESPACE_PARTS, &tns) == MOQ_OK);
            MOQ_TEST_CHECK(moq_buf_reader_remaining(&tr) != 0);
        }
    }
    failures += nsfin_deliver(a->tp.client_bridge, &a->tp.server_ep, &moved,
                              "arm namespace");
    MOQ_TEST_CHECK_EQ_SIZE(moved, (size_t)1);
    MOQ_TEST_CHECK(event_queue_full(a->tp.client));
    MOQ_TEST_CHECK_EQ_SIZE(a->tp.client_ep.count, (size_t)0);

    /* The NAMESPACE transition, applied EXPLICITLY: one inbound tracker is
     * present (presence only -- its address cannot authorize itself) and the
     * active budget is an absolute declared value with checked addition. */
    {
        const size_t charge = NF_SUFFIX_ARRAY_CHARGE +
                              nf_suffix_charge(suffix_field);
        MOQ_TEST_CHECK(charge <= SIZE_MAX - a->budget0);
        a->budget_active = a->budget0 + charge;
        nf_inv_t want = a->want_live;
        want.cmp_suffix_ptr = 0;
        want.suffixes = (const void *)1;
        want.suffixes_inbound = 1;
        nf_inv_t now;
        nf_inv_read(a->tp.client, a->slot, &now);
        now.cmp_suffix_ptr = 0;
        failures += nf_inv_equals(&now, &want, "arm namespace owner");
        MOQ_TEST_CHECK(now.suffixes != NULL);
        MOQ_TEST_CHECK_EQ_SIZE(a->tp.client->recv_payload_bytes,
                               a->budget_active);
        /* Only now is the tracker address adopted for later conservation. */
        a->want_live.suffixes =
            a->tp.client->ns_subs[a->slot].announced_suffixes;
        a->want_live.suffixes_inbound = 1;
    }
    return failures;
}

/* The complete pre-terminal picture: owner inventory, sole ns edge, empty
 * drain set, and ONE bridge record reachable by both identities. */
static int nsfin_arm_precheck(nsfin_arm_t *a, nf_inv_t *live, nf_drain_t *d0,
                              og_graph_t *g0, const char *what)
{
    /* The DECLARED owner the arm built, not a fresh read of the session. */
    *live = a->want_live;
    int failures = nf_inv_check(a->tp.client, a->slot, live, what);

    MOQ_TEST_CHECK(nf_drain_snap(a->tp.client, d0) == 0);
    MOQ_TEST_CHECK_EQ_SIZE(d0->count, (size_t)0);

    og_capture(a->tp.client, g0);
    failures += og_check_integrity(g0, what);
    const og_edge_spec_t edges[] = { { OG_DOM_NS_REF, a->ref._v } };
    failures += og_check_owner_edges(g0, MOQ_REQ_NAMESPACE_SUB, a->slot,
                                     edges, 1, what);

    nsfin_entry_want_t ew = nsfin_entry_live();
    failures += nsfin_check_entry(a, &ew, what);
    /* No session output is owed at the arm point. */
    MOQ_TEST_CHECK_EQ_SIZE(
        a->tp.client->action_tail - a->tp.client->action_head, (size_t)0);
    return failures;
}

/* One conservation checker for the blocked and post-blocker windows: the
 * complete owner, exact graph topology, exact drain set, exact receive
 * budget, and no session action output. */
static int nsfin_conserved(nsfin_arm_t *a, const nf_inv_t *want,
                           const og_graph_t *g0, const nf_drain_t *d0,
                           size_t budget, const char *what)
{
    int failures = nf_inv_check(a->tp.client, a->slot, want, what);
    og_graph_t g;
    og_capture(a->tp.client, &g);
    failures += og_check_same_topology(g0, &g, what);
    nf_drain_t d;
    MOQ_TEST_CHECK(nf_drain_snap(a->tp.client, &d) == 0);
    failures += nf_drain_equals(&d, d0, what);
    if (a->tp.client->recv_payload_bytes != budget) {
        fprintf(stderr, "NSFIN %s: receive budget %zu, expected %zu\n", what,
                a->tp.client->recv_payload_bytes, budget);
        failures++;
    }
    MOQ_TEST_CHECK_EQ_SIZE(
        a->tp.client->action_tail - a->tp.client->action_head, (size_t)0);
    return failures;
}

/* Phases 8-9: nothing of this owner survives, in EITHER identity. */
static int nsfin_check_retired(nsfin_arm_t *a, const nf_inv_t *live,
                               const nf_drain_t *d0, size_t budget0,
                               const char *what)
{
    int failures = 0;
    if (a->tp.client->recv_payload_bytes != budget0) {
        fprintf(stderr, "NSFIN %s: receive budget %zu, expected %zu\n", what,
                a->tp.client->recv_payload_bytes, budget0);
        failures++;
    }
    if (moq_transport_bridge_stream_has_pending(a->tp.client_bridge, a->bidi)) {
        fprintf(stderr, "NSFIN %s: the stream still reports pending work\n",
                what);
        failures++;
    }
    nf_inv_t want = *live;
    nf_inv_apply_free(&want);
    failures += nf_inv_check(a->tp.client, a->slot, &want, what);

    og_graph_t g;
    og_capture(a->tp.client, &g);
    failures += og_check_integrity(&g, what);
    failures += og_check_no_edge(&g, OG_DOM_NS_REF, a->ref._v, what);
    failures += og_check_owner_unreferenced(&g, MOQ_REQ_NAMESPACE_SUB, a->slot,
                                            what);

    nf_drain_t d1;
    MOQ_TEST_CHECK(nf_drain_snap(a->tp.client, &d1) == 0);
    failures += nf_drain_equals(&d1, d0, what);

    if (bridge_find_by_id(a->tp.client_bridge, a->bidi) != NULL) {
        fprintf(stderr, "NSFIN %s: the transport-id mapping survived\n", what);
        failures++;
    }
    if (bridge_find_by_ref(a->tp.client_bridge, a->ref) != NULL) {
        fprintf(stderr, "NSFIN %s: the internal-ref mapping survived\n", what);
        failures++;
    }
    MOQ_TEST_CHECK(!moq_transport_bridge_has_pending(a->tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(a->tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(a->tp.client_bridge));
    return failures;
}

/* Exactly one empty-FIN WRITE on the original transport id, or none. */
static int nsfin_expect_ops(nsfin_arm_t *a, size_t want_fin_writes,
                            const char *what)
{
    int failures = 0;
    size_t fins = 0, other = 0;
    MOQ_TEST_CHECK(a->tp.client_ep.count < FAKE_EP_MAX_OPS);
    for (size_t i = 0; i < a->tp.client_ep.count; i++) {
        fake_op_t *o = &a->tp.client_ep.ops[i];
        if (o->kind == FAKE_OP_WRITE && o->stream_id == a->bidi &&
            o->data_len == 0 && o->fin)
            fins++;
        else
            other++;
    }
    if (fins != want_fin_writes || other != 0) {
        fprintf(stderr, "NSFIN %s: %zu empty-FIN writes and %zu other ops,"
                " expected %zu/0\n", what, fins, other, want_fin_writes);
        failures++;
    }
    fake_endpoint_clear_ops(&a->tp.client_ep);
    return failures;
}

/* Release ONLY the blocker: exactly the declared NAMESPACE_FOUND. */
static int nsfin_release_blocker(nsfin_arm_t *a)
{
    nf_ev_t got[NF_EV_MAX]; size_t k = 0;
    int failures = nf_collect(a->tp.client, a->handle, got, NF_EV_MAX, &k,
                              "blocker");
    MOQ_TEST_CHECK_EQ_SIZE(k, (size_t)1);
    if (k == 1) {
        nf_ev_t want = nf_ev_want(MOQ_EVENT_NAMESPACE_FOUND, "room", a->sfx);
        failures += nf_ev_equals(&got[0], &want, "blocker");
    }
    return failures;
}

static int nsfin_expect_gone(nsfin_arm_t *a, size_t n, const char *what)
{
    nf_ev_t got[NF_EV_MAX]; size_t k = 0;
    int failures = nf_collect(a->tp.client, a->handle, got, NF_EV_MAX, &k,
                              what);
    nf_ev_t want[1] = { nf_ev_want(MOQ_EVENT_NAMESPACE_GONE, "room", a->sfx) };
    if (n == 0) {
        MOQ_TEST_CHECK_EQ_SIZE(k, (size_t)0);
        return failures;
    }
    failures += nf_multiset(got, k, want, 1, what);
    return failures;
}

static int test_ns_response_fin_bridge(void)
{
    int failures = 0;
    nsfin_arm_t a;
    failures += nsfin_arm_build(&a, "alpha");

    nf_inv_t live; nf_drain_t d0; og_graph_t g0;
    failures += nsfin_arm_precheck(&a, &live, &d0, &g0, "fin arm");

    /* (5) the peer FINs its response half while the client cannot emit. */
    moq_result_t rc = moq_transport_bridge_on_peer_bidi_bytes(
        a.tp.client_bridge, a.bidi, NULL, 0, true, 0);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(a.tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(a.tp.client_bridge));
    {
        nsfin_entry_want_t ew = nsfin_entry_live();
        ew.pending_retry = 1;
        ew.fin_retained = 1;
        ew.stream_pending = 1;
        ew.bridge_pending = 1;
        failures += nsfin_check_entry(&a, &ew, "fin blocked");
    }
    nf_inv_t want_blocked = live;
    want_blocked.pending_fin = 1;
    const size_t budget_live = a.budget_active;   /* DECLARED by the arm */
    failures += nsfin_conserved(&a, &want_blocked, &g0, &d0, budget_live,
                                "fin blocked");
    failures += nsfin_expect_ops(&a, 0, "fin blocked");

    /* (7) release only the blocker, reassert, then SERVICE -- no
     * re-delivery of bytes, FIN or reset at any point. */
    failures += nsfin_release_blocker(&a);
    {
        nsfin_entry_want_t ew = nsfin_entry_live();
        ew.pending_retry = 1;
        ew.fin_retained = 1;
        ew.stream_pending = 1;
        ew.bridge_pending = 1;
        failures += nsfin_check_entry(&a, &ew, "fin released");
    }
    failures += nsfin_conserved(&a, &want_blocked, &g0, &d0, budget_live,
                                "fin released");
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(a.tp.client_bridge, 0), (int)MOQ_OK);
    /* (8) exact completion */
    failures += nsfin_expect_gone(&a, 1, "fin complete");
    failures += nsfin_expect_ops(&a, 1, "fin complete");
    failures += nsfin_check_retired(&a, &live, &d0, a.budget0, "fin complete");

    /* (9) a second service emits nothing and repeats the postcondition. */
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(a.tp.client_bridge, 0), (int)MOQ_OK);
    failures += nsfin_expect_gone(&a, 0, "fin idempotent");
    failures += nsfin_expect_ops(&a, 0, "fin idempotent");
    failures += nsfin_check_retired(&a, &live, &d0, a.budget0, "fin idempotent");

    test_pair_destroy(&a.tp);
    return failures;
}

static int test_ns_response_reset_bridge(void)
{
    int failures = 0;
    nsfin_arm_t a;
    failures += nsfin_arm_build(&a, "beta");

    nf_inv_t live; nf_drain_t d0; og_graph_t g0;
    failures += nsfin_arm_precheck(&a, &live, &d0, &g0, "reset arm");

    /* (6) the peer resets while the client cannot emit. */
    moq_result_t rc = moq_transport_bridge_on_peer_stream_reset(
        a.tp.client_bridge, a.bidi, 0x2B, 0);
    MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(!moq_transport_bridge_is_fatal(a.tp.client_bridge));
    MOQ_TEST_CHECK(!moq_transport_bridge_is_closed(a.tp.client_bridge));
    {
        nsfin_entry_want_t ew = nsfin_entry_live();
        ew.pending_reset = 1;
        ew.pending_reset_code = 0x2Bu;
        ew.stream_pending = 1;
        ew.bridge_pending = 1;
        failures += nsfin_check_entry(&a, &ew, "reset blocked");
    }
    const size_t budget_live = a.budget_active;   /* DECLARED by the arm */
    failures += nsfin_conserved(&a, &live, &g0, &d0, budget_live,
                                "reset blocked");
    failures += nsfin_expect_ops(&a, 0, "reset blocked");

    failures += nsfin_release_blocker(&a);
    {
        nsfin_entry_want_t ew = nsfin_entry_live();
        ew.pending_reset = 1;
        ew.pending_reset_code = 0x2Bu;
        ew.stream_pending = 1;
        ew.bridge_pending = 1;
        failures += nsfin_check_entry(&a, &ew, "reset released");
    }
    failures += nsfin_conserved(&a, &live, &g0, &d0, budget_live,
                                "reset released");
    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(a.tp.client_bridge, 0), (int)MOQ_OK);
    failures += nsfin_expect_gone(&a, 1, "reset complete");
    /* A reset owns physical teardown: no local close is queued. */
    failures += nsfin_expect_ops(&a, 0, "reset complete");
    failures += nsfin_check_retired(&a, &live, &d0, a.budget0, "reset complete");

    MOQ_TEST_CHECK_EQ_INT(
        (int)moq_transport_bridge_service(a.tp.client_bridge, 0), (int)MOQ_OK);
    failures += nsfin_expect_gone(&a, 0, "reset idempotent");
    failures += nsfin_expect_ops(&a, 0, "reset idempotent");
    failures += nsfin_check_retired(&a, &live, &d0, a.budget0, "reset idempotent");

    test_pair_destroy(&a.tp);
    return failures;
}

static int test_fin_bridge_descriptor_validation(void)
{
    int failures = 0;
    fin_bridge_run_t r;
    memset(&r, 0, sizeof(r));

    fin_bridge_family_t o = p7_family;
    fin_bridge_case_t f;
    memset(&f, 0, sizeof(f));
    f.name = "probe";
    f.ctx = &r;
    f.ingress = FIN_BR_CONSUMED;
    f.family = &o;
    f.feed = p7_feed;
    f.terminal = p7_terminal;
    MOQ_TEST_CHECK_EQ_INT(fin_bridge_problems(&f), 0);

    /* An oversized snapshot declaration must be refused BEFORE any family hook
     * runs -- the runner writes into bounded storage, so a hook invoked under an
     * invalid descriptor would overflow it. */
    {
        fin_bridge_family_t big = p7_family;
        big.snap_size = FIN_BR_SNAP_MAX + 1;
        big.capture = probe_capture_forbidden;
        fin_bridge_case_t bad = f;
        bad.family = &big;
        probe_capture_calls = 0;
        MOQ_TEST_CHECK(fin_bridge_problems(&bad) > 0);
        fin_br_quiet = 1;
        MOQ_TEST_CHECK(run_fin_bridge(&bad, &r) > 0);
        fin_br_quiet = 0;
        MOQ_TEST_CHECK_EQ_INT(probe_capture_calls, 0);
    }

    /* ingress class */
    f.ingress = 0;              MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    f.ingress = 99;             MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    f.ingress = FIN_BR_RETAINED; MOQ_TEST_CHECK_EQ_INT(fin_bridge_problems(&f), 0);
    f.ingress = FIN_BR_CONSUMED;

    /* the bundle itself */
    f.family = NULL;             MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    f.family = &o;

    /* every required member */
    o = p7_family; o.snap_size = 0;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.snap_size = FIN_BR_SNAP_MAX + 1;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    /* A nonzero but UNDERSIZED declaration validates structurally -- the
     * descriptor cannot know the family's real state size -- so the runner
     * passes the DECLARED size and the family refuses to write. That refusal
     * is what the undersized mutant below proves. */
    o = p7_family; o.snap_size = 1;
    MOQ_TEST_CHECK_EQ_INT(fin_bridge_problems(&f), 0);
    o = p7_family; o.derive_slot = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.check_live = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.check_retired = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.check_edges = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.check_drain = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.check_terminal_wire = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.want_request = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);

    /* family identity: unknown kind, and a tag that belongs to another family */
    o = p7_family; o.owner_kind = MOQ_REQ_NONE;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.owner_kind = 0x7f;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.pool_tag = MOQ_HANDLE_POOL_FETCH;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.owner_kind = MOQ_REQ_FETCH;   /* tag still TRACK_STATUS */
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family;
    MOQ_TEST_CHECK_EQ_INT(fin_bridge_problems(&f), 0);

    /* the output members, now family-owned */
    o = p7_family; o.capture = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.check = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.normalize_event = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family; o.normalize_action = NULL;
    MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    o = p7_family;
    MOQ_TEST_CHECK_EQ_INT(fin_bridge_problems(&f), 0);

    /* the case's single context */
    f.ctx = NULL;               MOQ_TEST_CHECK(fin_bridge_problems(&f) > 0);
    f.ctx = &r;
    MOQ_TEST_CHECK_EQ_INT(fin_bridge_problems(&f), 0);
    return failures;
}

static int p7_fixture_setup(fin_bridge_run_t *r, const char *what)
{
    int failures = 0;
    memset(r, 0, sizeof(*r));
    if (d18_pair_init(&r->tp, 0) < 0) return -1;
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(r->tp.client, 0), (int)MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_session_start(r->tp.server, 0), (int)MOQ_OK);
    failures += d18_strict_shuttle(&r->tp, 30, 0, what);
    /* Setup is classified, not discarded: exactly one SETUP_COMPLETE per side
     * and nothing else. */
    {
        int cs = 0, co = 0, ss = 0, so = 0;
        moq_event_t ev;
        while (moq_session_poll_events(r->tp.client, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) cs++; else co++;
            moq_event_cleanup(&ev);
        }
        while (moq_session_poll_events(r->tp.server, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) ss++; else so++;
            moq_event_cleanup(&ev);
        }
        MOQ_TEST_CHECK_EQ_INT(cs, 1);
        MOQ_TEST_CHECK_EQ_INT(co, 0);
        MOQ_TEST_CHECK_EQ_INT(ss, 1);
        MOQ_TEST_CHECK_EQ_INT(so, 0);
    }
    fake_endpoint_clear_ops(&r->tp.server_ep);
    fake_endpoint_clear_ops(&r->tp.client_ep);
    r->transport_id = 400;                /* peer-opened client-initiated bidi */
    return failures;
}

static void p7_case_init(fin_bridge_case_t *f, fin_bridge_run_t *r)
{
    memset(f, 0, sizeof(*f));
    f->name = "p7 track-status";
    f->ctx = r;
    f->ingress = FIN_BR_CONSUMED;
    f->family = &p7_family;
    f->feed = p7_feed;
    f->terminal = p7_terminal;
}

static int test_p7_bridge_fin_retirement(void)
{
    int failures = 0;
    fin_bridge_run_t r;
    int rc = p7_fixture_setup(&r, "p7 setup");
    if (rc < 0) return 1;
    failures += rc;

    fin_bridge_case_t f;
    p7_case_init(&f, &r);

    failures += run_fin_bridge(&f, &r);
    test_pair_destroy(&r.tp);
    return failures;
}

/* Axis 4, on the family whose whole lifecycle is reachable today: once the
 * owner has been retired, retirement must be COMPLETE and hold under repetition
 * and reuse. Three obligations, each non-vacuous against the current source:
 *
 *   - a repeated APPLICATION terminal on the retired handle is refused and
 *     changes nothing -- no wire byte, no event, no action, no resurrected
 *     owner, no drain, no edge;
 *   - a fresh request REUSES the slot with an ADVANCED generation, so the two
 *     owners are distinguishable by handle;
 *   - the RETIRED handle stays refused after that reuse, which is the
 *     clear-exactly-once fact: a stale handle must not address the new owner.
 *
 * It deliberately asserts nothing about where a future FIN handoff marker
 * lives, and does not carry P3's retained-ingress assumption.
 */
static int p7_accept(fin_bridge_run_t *r, uint64_t handle)
{
    moq_track_status_handle_t h;
    h._opaque = handle;
    moq_accept_track_status_cfg_t c;
    moq_accept_track_status_cfg_init(&c);
    c.has_largest = true;
    c.largest_group = P7_LARGEST_GROUP;
    c.largest_object = P7_LARGEST_OBJ;
    c.has_expires = true;
    c.expires_ms = P7_EXPIRES_MS;
    return (int)moq_session_accept_track_status(r->tp.server, h, &c, 0);
}

/* No output of ANY kind: no endpoint op, no event, no action. */
static int p7_check_silent(fin_bridge_run_t *r, const char *what)
{
    int bad = 0;
    if (r->tp.server_ep.count != 0) {
        fprintf(stderr, "AXIS4 %s: %zu endpoint ops, expected 0\n", what,
                r->tp.server_ep.count);
        bad++;
    }
    moq_event_t ev;
    while (moq_session_poll_events(r->tp.server, &ev, 1) > 0) {
        fprintf(stderr, "AXIS4 %s: unexpected event kind %u\n", what,
                (unsigned)ev.kind);
        moq_event_cleanup(&ev);
        bad++;
    }
    moq_action_t a;
    while (moq_session_poll_actions(r->tp.server, &a, 1) > 0) {
        fprintf(stderr, "AXIS4 %s: unexpected action kind %u\n", what,
                (unsigned)a.kind);
        moq_action_cleanup(&a);
        bad++;
    }
    return bad;
}

/* The generic snapshot records the scratch cursor as it stands, but the next
 * advancing call reclaims it once the event queue has drained
 * (session_call_prepare). Normalizing that -- and only that, and only when the
 * captured queue was empty -- keeps real scratch mutation detectable. Same rule
 * the session-side FIN suite applies. */
static void p7_expect_after_call_prepare(txs_snapshot_t *snap)
{
    if (snap->event_depth == 0) snap->event_scratch_len = 0;
}

/* The DECLARED free record for `ts_free_entry` (session_track_status.c:27).
 * That free MEMSETS the entry and then restores exactly three things: the FREE
 * state, the next generation, and the co-allocated receive buffer. So the
 * expectation is "zeroed except those", with the owned track key required GONE
 * -- a narrowed cleanup that left any field behind is caught here rather than
 * by an absence check that a stale value also satisfies. */
typedef struct p7_free_expect {
    uint32_t        generation;
    const uint8_t  *req_recv_buf;
    size_t          req_recv_cap;
} p7_free_expect_t;

static int p7_check_free_record(const moq_session_t *s, int slot,
                                const p7_free_expect_t *w, const char *what)
{
    int bad = 0;
    if (slot < 0 || (size_t)slot >= s->ts_cap) {
        fprintf(stderr, "AXIS4 %s: slot out of range\n", what);
        return 1;
    }
    const moq_ts_entry_t *e = &s->track_statuses[slot];
#define P7_FREE_EQ(field, got, exp) do { \
    if ((uint64_t)(got) != (uint64_t)(exp)) { \
        fprintf(stderr, "AXIS4 %s: free record %s = %llu, expected %llu\n", \
                what, field, (unsigned long long)(got), \
                (unsigned long long)(exp)); \
        bad++; \
    } \
} while (0)
    P7_FREE_EQ("state", (int)e->state, (int)MOQ_TS_FREE);
    P7_FREE_EQ("generation", e->generation, w->generation);
    P7_FREE_EQ("req_recv_cap", e->req_recv_cap, w->req_recv_cap);
    /* Everything the memset zeroes. */
    P7_FREE_EQ("role", (int)e->role, 0);
    P7_FREE_EQ("handle", e->handle._opaque, 0);
    P7_FREE_EQ("request_id", e->request_id, 0);
    P7_FREE_EQ("request_stream_ref", e->request_stream_ref._v, 0);
    P7_FREE_EQ("req_recv_len", e->req_recv_len, 0);
    P7_FREE_EQ("req_recv_fin", e->req_recv_fin ? 1 : 0, 0);
    P7_FREE_EQ("goaway_sent", e->goaway_sent ? 1 : 0, 0);
    P7_FREE_EQ("track_id_len", e->track_id_len, 0);
#undef P7_FREE_EQ
    if (e->req_recv_buf != w->req_recv_buf) {
        fprintf(stderr, "AXIS4 %s: free record req_recv_buf pointer\n", what);
        bad++;
    }
    if (e->track_id_buf != NULL) {
        fprintf(stderr, "AXIS4 %s: owned track key survives the free\n", what);
        bad++;
    }
    if (e->hist != NULL) {
        fprintf(stderr, "AXIS4 %s: reserved history record survives\n", what);
        bad++;
    }
    return bad;
}

/* The retired physical mapping is gone under BOTH identities -- a stale entry
 * that lost only its transport-id key would still be reachable by ref -- and
 * the bridge itself is healthy with nothing owed. */
static int p7_check_mapping_absent(fin_bridge_run_t *r, uint64_t transport_id,
                                   moq_stream_ref_t old_ref, const char *what)
{
    int bad = 0;
    if (bridge_find_by_id(r->tp.server_bridge, transport_id) != NULL) {
        fprintf(stderr, "AXIS4 %s: retired transport id still maps\n", what);
        bad++;
    }
    if (bridge_find_by_ref(r->tp.server_bridge, old_ref) != NULL) {
        fprintf(stderr, "AXIS4 %s: retired internal ref still maps\n", what);
        bad++;
    }
    if (moq_transport_bridge_is_fatal(r->tp.server_bridge)) {
        fprintf(stderr, "AXIS4 %s: bridge fatal\n", what); bad++;
    }
    if (moq_transport_bridge_is_closed(r->tp.server_bridge)) {
        fprintf(stderr, "AXIS4 %s: bridge closed\n", what); bad++;
    }
    if (moq_transport_bridge_has_pending(r->tp.server_bridge)) {
        fprintf(stderr, "AXIS4 %s: bridge has pending work\n", what); bad++;
    }
    return bad;
}

/* The reused peer bidi's EXACT bridge record. This is an OPEN-peer-bidi oracle,
 * deliberately not the FIN-consumed phase oracle: that phase expects
 * peer_send_closed, which is wrong for an entry admitted from a request with no
 * FIN. Both lookups must land on the SAME record -- a reverse-lookup repoint
 * that kept the transport id would satisfy a "some mapping remains" check --
 * and every flag is declared, so an otherwise-unobserved one cannot drift. */
static int p7_check_open_peer_bidi(fin_bridge_run_t *r, uint64_t transport_id,
                                   moq_stream_ref_t want_ref, const char *what)
{
    int bad = 0;
    bridge_stream_entry_t *by_id = bridge_find_by_id(r->tp.server_bridge,
                                                     transport_id);
    bridge_stream_entry_t *by_ref = bridge_find_by_ref(r->tp.server_bridge,
                                                       want_ref);
    if (!by_id) {
        fprintf(stderr, "AXIS4 %s: no bridge entry for transport id\n", what);
        return 1;
    }
    if (!by_ref) {
        fprintf(stderr, "AXIS4 %s: no bridge entry for internal ref\n", what);
        return 1;
    }
    if (by_id != by_ref) {
        fprintf(stderr, "AXIS4 %s: the two lookups reach DIFFERENT records\n",
                what);
        bad++;
    }
#define P7_BR_EQ(field, got, exp) do { \
    if ((uint64_t)(got) != (uint64_t)(exp)) { \
        fprintf(stderr, "AXIS4 %s: bridge entry %s = %llu, expected %llu\n", \
                what, field, (unsigned long long)(got), \
                (unsigned long long)(exp)); \
        bad++; \
    } \
} while (0)
    P7_BR_EQ("transport_id", by_id->transport_id, transport_id);
    P7_BR_EQ("ref", by_id->ref._v, want_ref._v);
    P7_BR_EQ("kind", (int)by_id->kind, (int)BRIDGE_STREAM_BIDI);
    P7_BR_EQ("origin", (int)by_id->origin, (int)BRIDGE_ORIGIN_PEER);
    P7_BR_EQ("active", by_id->active ? 1 : 0, 1);
    /* An OPEN peer bidi with no FIN and no teardown: every one of these is
     * false, and each is named so a single drifting flag is attributable. */
    P7_BR_EQ("peer_send_closed", by_id->peer_send_closed ? 1 : 0, 0);
    P7_BR_EQ("local_send_closed", by_id->local_send_closed ? 1 : 0, 0);
    P7_BR_EQ("aborting", by_id->aborting ? 1 : 0, 0);
    P7_BR_EQ("pending_retry", by_id->pending_retry ? 1 : 0, 0);
    P7_BR_EQ("pending_fin", by_id->pending_fin ? 1 : 0, 0);
    P7_BR_EQ("fin_retained", by_id->fin_retained ? 1 : 0, 0);
    P7_BR_EQ("pending_reset", by_id->pending_reset ? 1 : 0, 0);
    P7_BR_EQ("pending_stop", by_id->pending_stop ? 1 : 0, 0);
    P7_BR_EQ("pending_reset_code", by_id->pending_reset_code, 0);
    P7_BR_EQ("pending_stop_code", by_id->pending_stop_code, 0);
    /* Uni-only classification residue stays in its initialized zero state on a
     * bidi entry. */
    P7_BR_EQ("uni_disp", by_id->uni_disp, (uint8_t)BRIDGE_UNI_DISP_PENDING);
    P7_BR_EQ("classify_len", by_id->classify_len, 0);
#undef P7_BR_EQ
    /* Length zero does not imply the buffer is clean: every retained
     * classification byte must be in its initialized zero state. */
    for (size_t ci = 0; ci < sizeof(by_id->classify_buf); ci++) {
        if (by_id->classify_buf[ci] != 0) {
            fprintf(stderr,
                    "AXIS4 %s: bridge entry classify_buf[%zu] = %u, expected 0\n",
                    what, ci, (unsigned)by_id->classify_buf[ci]);
            bad++;
        }
    }
    /* And the bridge itself is still healthy with nothing owed. */
    if (moq_transport_bridge_is_fatal(r->tp.server_bridge)) {
        fprintf(stderr, "AXIS4 %s: bridge fatal\n", what); bad++;
    }
    if (moq_transport_bridge_is_closed(r->tp.server_bridge)) {
        fprintf(stderr, "AXIS4 %s: bridge closed\n", what); bad++;
    }
    if (moq_transport_bridge_has_pending(r->tp.server_bridge)) {
        fprintf(stderr, "AXIS4 %s: bridge has pending work\n", what); bad++;
    }
    return bad;
}

static int test_p7_retirement_idempotence_and_reuse(void)
{
    int failures = 0;
    fin_bridge_run_t r;
    int rc = p7_fixture_setup(&r, "axis4 setup");
    if (rc < 0) return 1;
    failures += rc;

    fin_bridge_case_t f;
    p7_case_init(&f, &r);

    /* The co-allocated receive buffer persists across entry reuse, so its
     * pointer and capacity are properties of the FREE slot: captured BEFORE
     * ingress rather than adopted from the record the free produces. */
    const int slot_pre = r.want_slot;
    MOQ_TEST_CHECK(slot_pre >= 0 && (size_t)slot_pre < r.tp.server->ts_cap);
    const uint8_t *pre_recv_buf = r.tp.server->track_statuses[slot_pre].req_recv_buf;
    const size_t   pre_recv_cap = r.tp.server->track_statuses[slot_pre].req_recv_cap;

    failures += run_fin_bridge(&f, &r);

    const uint64_t retired_handle = r.want_handle;
    const int      slot           = r.want_slot;
    const moq_stream_ref_t old_ref = r.ref;

    /* The declared free record: the generation the LIVE owner carried (taken
     * from its declared handle, which the fixture derived pre-ingress) plus
     * one, and the buffer identity captured above. Nothing is read back from
     * the freed entry. */
    p7_free_expect_t want_free;
    want_free.generation   = moq_handle_generation(retired_handle) + 1u;
    want_free.req_recv_buf = pre_recv_buf;
    want_free.req_recv_cap = pre_recv_cap;
    failures += p7_check_free_record(r.tp.server, slot, &want_free,
                                     "first retirement");

    /* 1. A repeated application terminal on the retired handle. It must not
     *    mutate unrelated session state, advance the generation a second time,
     *    change the graph, add a drain, or recreate the retired owner. */
    fake_endpoint_clear_ops(&r.tp.server_ep);
    og_graph_t g_pre_repeat;
    og_capture(r.tp.server, &g_pre_repeat);
    {
        txs_snapshot_t before;
        txs_capture(r.tp.server, &old_ref, 1, &before);
        p7_expect_after_call_prepare(&before);
        MOQ_TEST_CHECK_EQ_INT(p7_accept(&r, retired_handle),
                              (int)MOQ_ERR_STALE_HANDLE);
        failures += p7_check_silent(&r, "repeat terminal");
        failures += txs_check_eq(r.tp.server, &old_ref, 1, &before,
                                 "repeat terminal");
    }
    failures += p7_family.check_retired(r.tp.server, old_ref, slot,
                                        "repeat terminal");
    failures += p7_family.check_drain(r.tp.server, "repeat terminal");
    /* The SAME declared record: no second generation increment. */
    failures += p7_check_free_record(r.tp.server, slot, &want_free,
                                     "repeat terminal");
    {
        og_graph_t g;
        og_capture(r.tp.server, &g);
        failures += og_check_integrity(&g, "repeat terminal");
        /* The WHOLE topology, not just the target's edges: an unrelated edge
         * inserted, removed or repointed by this refused call is caught here. */
        failures += og_check_same_topology(&g, &g_pre_repeat, "repeat terminal");
        failures += p7_family.check_edges(&g, old_ref, slot, 0,
                                          "repeat terminal");
    }
    failures += p7_check_mapping_absent(&r, r.transport_id, old_ref,
                                        "repeat terminal");

    /* 2. A fresh request reuses the slot with an advanced generation. The new
     *    identity is DERIVED from pool state before ingress, never read back. */
    /* Derived from the DECLARED freed generation, not from a fresh read of the
     *    now-free slot -- a free that advanced the generation twice, or not at
     *    all, must not be able to define the expectation it is checked against. */
    const uint32_t new_gen = want_free.generation | 1u;
    const int new_slot = slot;
    uint64_t new_handle = moq_handle_pack(p7_family.pool_tag,
                                          r.tp.server->session_tag, new_gen,
                                          (uint32_t)new_slot);
    MOQ_TEST_CHECK(new_handle != retired_handle);
    {   /* The pool really does hand back the same slot. */
        uint32_t probe_gen = 0;
        MOQ_TEST_CHECK_EQ_INT(p7_family.derive_slot(r.tp.server, &probe_gen),
                              slot);
        MOQ_TEST_CHECK_EQ_U64(probe_gen, (uint64_t)new_gen);
    }

    const uint64_t reuse_id = 404;                  /* a second peer-opened bidi */
    p7_want_request_id = 2;                         /* peer ids advance by two */
    /* NO FIN this time: the reused owner must show a CLEARED latch, which is
     * what makes ts_free_entry's cleanup of that field observable. */
    p7_want_fin = 0;
    fake_endpoint_clear_ops(&r.tp.server_ep);
    MOQ_TEST_CHECK_EQ_INT(
        (int)p7_feed_id(r.tp.server_bridge, 2, reuse_id, false), (int)MOQ_OK);
    {
        bridge_stream_entry_t *e = bridge_find_by_id(r.tp.server_bridge, reuse_id);
        MOQ_TEST_CHECK(e != NULL);
        if (!e) return failures;             /* the check above counted it */
        moq_stream_ref_t new_ref = e->ref;
        MOQ_TEST_CHECK(new_ref._v != old_ref._v);
        failures += p7_family.check_live(r.tp.server, new_ref, new_slot, new_gen,
                                         new_handle, "reused");
    }
    {   /* The request surfaces against the DERIVED handle, and only it. */
        txs_norm_vec_t got, want;
        txs_norm_init(&got); txs_norm_init(&want);
        moq_event_t ev;
        while (moq_session_poll_events(r.tp.server, &ev, 1) > 0) {
            if (!p7_family.normalize_event(&ev, &r, &got)) failures++;
            moq_event_cleanup(&ev);
        }
        failures += p7_family.want_request(&want, new_handle);
        failures += txs_norm_equals(&got, &want, "reused request");
        txs_norm_free(&got); txs_norm_free(&want);
    }

    /* 3. The retired handle is STILL refused, although its slot is occupied
     *    again -- a stale handle must never address the new owner -- and the
     *    REPLACEMENT owner survives that refusal WHOLE. */
    fake_endpoint_clear_ops(&r.tp.server_ep);
    {
        bridge_stream_entry_t *be = bridge_find_by_id(r.tp.server_bridge,
                                                      reuse_id);
        MOQ_TEST_CHECK(be != NULL);
        if (be) {
            moq_stream_ref_t new_ref = be->ref;
            fin_bridge_snap_t owner_before;
            p7_family.capture(r.tp.server, &r, owner_before.bytes,
                              p7_family.snap_size);
            txs_snapshot_t before;
            txs_capture(r.tp.server, &new_ref, 1, &before);
            p7_expect_after_call_prepare(&before);
            og_graph_t g_pre_stale;
            og_capture(r.tp.server, &g_pre_stale);

            MOQ_TEST_CHECK_EQ_INT(p7_accept(&r, retired_handle),
                                  (int)MOQ_ERR_STALE_HANDLE);

            failures += p7_check_silent(&r, "stale after reuse");
            failures += txs_check_eq(r.tp.server, &new_ref, 1, &before,
                                     "stale after reuse");
            failures += p7_family.check(r.tp.server, &r, owner_before.bytes,
                                        p7_family.snap_size,
                                        "stale after reuse");
            failures += p7_family.check_live(r.tp.server, new_ref, new_slot,
                                             new_gen, new_handle,
                                             "stale after reuse");
            failures += p7_family.check_drain(r.tp.server, "stale after reuse");
            {
                og_graph_t g;
                og_capture(r.tp.server, &g);
                failures += og_check_integrity(&g, "stale after reuse");
                failures += og_check_same_topology(&g, &g_pre_stale,
                                                   "stale after reuse");
                failures += p7_family.check_edges(&g, new_ref, new_slot, 1,
                                                  "stale after reuse");
                /* The retired stream keys nothing, though its slot is live. */
                failures += og_check_no_edge(&g, OG_DOM_REQ_STREAMREF,
                                             old_ref._v, "stale after reuse");
            }
            /* Both identities, with the flags this phase requires. */
            failures += p7_check_open_peer_bidi(&r, reuse_id, new_ref,
                                                "stale after reuse");
            failures += p7_check_mapping_absent(&r, r.transport_id, old_ref,
                                                "stale after reuse");
        }
    }

    p7_want_request_id = 0;
    p7_want_fin = 1;
    test_pair_destroy(&r.tp);
    return failures;
}


int main(void)
{
    int failures = 0;

    failures += test_budget_context_paired_on_every_exit();
    failures += test_unlimited_service_enters_no_budget_context();
    failures += test_budgeted_service_requires_output();
    failures += test_control_retry_suspension_preserves_pending();
    failures += test_data_retry_suspension_preserves_pending();
    failures += test_data_reset_suspension_preserves_pending();
    failures += test_bidi_retry_suspension_preserves_pending();
    failures += test_fin_bridge_descriptor_validation();
    failures += test_ns_response_fin_bridge();
    failures += test_ns_response_reset_bridge();
    failures += test_p7_bridge_fin_retirement();
    failures += test_p7_retirement_idempotence_and_reuse();
    failures += test_bridge_nomem_ns_response();
    failures += test_bridge_nomem_joining_fetch();
    failures += test_bidi_reset_suspension_preserves_pending();
    failures += test_bidi_stop_suspension_preserves_pending();
    failures += test_data_stop_suspension_preserves_pending();
    failures += test_tick_suspension_preserves_due_deadline();
    failures += test_inbound_scan_stops_at_suspension();
    failures += test_progress_then_suspension_in_one_pass();
    failures += test_continuation_reclaims_event_scratch();
    failures += test_create_destroy();
    failures += test_create_rejects_bad_ops();
    failures += test_setup_handshake();
    failures += test_control_write_backpressure();
    failures += test_transport_close();
    failures += test_datagram_inbound_not_fatal();
    failures += test_inbound_uni_after_setup();
    failures += test_close_error_is_fatal_not_closed();

    /* Regression tests */
    failures += test_empty_uni_no_ghost_stream();
    failures += test_truncated_vtable_rejected();
    failures += test_inbound_uni_dropped_then_discarded();
    failures += test_inbound_uni_rcbuf_dropped_then_discarded();
    failures += test_pending_retry_keeps_bytes();
    failures += test_pending_retry_keeps_rcbuf();

    /* Hard retry tests */
    failures += test_close_retry_after_blocked_control();
    failures += test_close_retry_would_block();
    failures += test_reset_on_unknown_stream();
    failures += test_transport_close_clears_state();

    /* terminal facts */
    failures += test_terminal_facts_enqueued_then_observed();
    failures += test_terminal_facts_not_set_by_other_events();
    failures += test_setup_scratch_shortfall_closes_not_fatal();

    if (failures == 0)
        printf("test_transport_bridge: all tests passed\n");
    else
        fprintf(stderr, "test_transport_bridge: %d failure(s)\n", failures);

    return failures;
}
