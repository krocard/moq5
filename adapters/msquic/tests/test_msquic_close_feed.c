/*
 * The transport terminal must always reach the SESSION, exactly once.
 *
 * The adapter records a transport close and feeds it to the bridge, which
 * turns it into the session's one MOQ_EVENT_SESSION_CLOSED. A managed child is
 * only reclaimable once the application has OBSERVED and acknowledged that
 * event, so a terminal that never reaches the session strands the child and
 * its admission reserve for the life of the facade.
 *
 * The case that matters is a bridge that already latched a terminal OF ITS OWN
 * -- an endpoint op that failed hard while the bridge was dispatching. That
 * latch tells the session nothing, so the real transport terminal which
 * follows is the only terminal the session will ever be offered. Both bridge
 * terminal flavors are covered: an unclean transport completion and an
 * orderly peer application close. The bridge's first fatal cause must win in
 * both cases.
 *
 * Deterministic and socket-free: one thread, the production connection
 * callback and service entries over the fake QUIC_API_TABLE. No sleeps, no
 * elapsed time, no repetition; every ordering is forced by the call sequence.
 *
 * Usage: test_msquic_close_feed
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "msquic_internal.h"

#include "support/fake_msq_table.h"

#include <moq/session.h>
#include <moq/transport_bridge.h>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* The code the app close carries in the stop control. */
#define LOCAL_CLOSE_CODE 0x7u
#define PEER_CLOSE_CODE 0x11u

/* --- rig ---------------------------------------------------------------- */

struct rig {
    fake_msq_t fake;
    moq_session_t *session;
    moq_msquic_conn_t *conn;
};

static int rig_up(struct rig *r, moq_msquic_hook_fn hook, void *hook_user)
{
    moq_session_cfg_t scfg;
    moq_msquic_conn_cfg_t cfg;

    memset(r, 0, sizeof(*r));
    fake_msq_init(&r->fake, true);
    moq_session_cfg_init_sized(&scfg, sizeof(scfg), moq_alloc_default(),
                               MOQ_PERSPECTIVE_CLIENT);
    if (moq_session_create(&scfg, 0, &r->session) < 0)
        return -1;
    moq_msquic_conn_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.session = r->session;
    cfg.api = fake_msq_table(&r->fake);
    cfg.hook = hook;
    cfg.hook_user = hook_user;
    if (moq_msquic_conn_create(&cfg, &r->conn) != MOQ_OK)
        return -1;
    if (moq_msquic_conn_bind(r->conn, fake_msq_conn_handle(&r->fake)) !=
        MOQ_OK)
        return -1;
    return 0;
}

static void rig_down(struct rig *r)
{
    if (r->conn != NULL)
        moq_msquic_conn_destroy(r->conn);
    if (r->session != NULL)
        moq_session_destroy(r->session);
}

static void deliver_conn_event(struct rig *r, QUIC_CONNECTION_EVENT_TYPE t)
{
    QUIC_CONNECTION_EVENT ev;

    memset(&ev, 0, sizeof(ev));
    ev.Type = t;
    moq_msquic_conn_callback()(fake_msq_conn_handle(&r->fake), r->conn, &ev);
}

/* Drain the session's events; report how many terminals came out and the code
 * the last one carried. */
struct drained {
    int total;
    int closed;
    uint64_t close_code;
};

static struct drained drain_events(struct rig *r)
{
    struct drained d;
    moq_event_t ev;

    memset(&d, 0, sizeof(d));
    while (moq_session_poll_events(r->session, &ev, 1) > 0) {
        d.total++;
        if (ev.kind == MOQ_EVENT_SESSION_CLOSED) {
            d.closed++;
            d.close_code = ev.u.closed.code;
        }
        moq_event_cleanup(&ev);
    }
    return d;
}

static void print_state(const char *tag, struct rig *r)
{
    bool obs = false;
    bool enq = moq_transport_bridge_terminal_facts(r->conn->bridge, &obs);

    printf("  %-22s fatal=%d closed=%d close_pending=%d close_fed=%d "
           "shutdown_complete=%d sess=%d enqueued=%d observed=%d "
           "shutdowns=%d feeds=%u\n",
           tag, (int)moq_msquic_conn_is_fatal(r->conn),
           (int)moq_transport_bridge_is_closed(r->conn->bridge),
           (int)r->conn->close_pending, (int)r->conn->close_fed,
           (int)r->conn->shutdown_complete,
           (int)moq_session_state(r->session), (int)enq, (int)obs,
           r->fake.conn_shutdowns, r->conn->test_close_feed_commits);
}

/* The whole point, asserted the same way everywhere: after the transport
 * terminal the session has been told exactly once, and the adapter fed it
 * exactly once. */
static void check_told_once(struct rig *r, struct drained d,
                            uint64_t expect_code)
{
    bool obs = false;
    bool enq = moq_transport_bridge_terminal_facts(r->conn->bridge, &obs);

    CHECK(r->conn->close_fed);
    CHECK(r->conn->test_close_feed_commits == 1);
    CHECK(enq);                          /* the session really queued it */
    CHECK(obs);                          /* ... and the drain transferred it */
    CHECK(d.total == 1);
    CHECK(d.closed == 1);
    CHECK(d.close_code == expect_code);
    CHECK(moq_session_state(r->session) == MOQ_SESS_CLOSED);
}

/* No further terminal can appear, however hard the connection is serviced. */
static void check_no_second_terminal(struct rig *r)
{
    struct drained again;

    moq_msquic_conn_service(r->conn);
    deliver_conn_event(r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    moq_msquic_conn_service(r->conn);
    again = drain_events(r);
    CHECK(again.total == 0);
    CHECK(again.closed == 0);
    CHECK(r->conn->test_close_feed_commits == 1);
}

/* --- 1. the defect: an internal fatal swallows the transport terminal ----- */

/*
 * The bridge latches its own fatal when an endpoint op fails hard mid-dispatch
 * -- here a StreamStart the transport refuses, exactly as one racing a closing
 * connection does. That latch is not a session terminal: the session is still
 * open with nothing queued. The transport terminal that follows is its last
 * notice and must be delivered.
 */
static void arm_internal_fatal(struct rig *r)
{
    /* the client's SETUP wants its control bidi; the transport refuses it */
    r->fake.stream_start_fails = 1;
    deliver_conn_event(r, QUIC_CONNECTION_EVENT_CONNECTED);
    print_state("after-connected", r);

    /* The bridge owns a first fatal cause, but the session still has no
     * terminal. This is the exact precondition the later transport close must
     * repair rather than treating bridge-fatal as session-closed. */
    bool obs0 = false;
    bool enq0 = moq_transport_bridge_terminal_facts(r->conn->bridge, &obs0);
    struct drained d0 = drain_events(r);

    CHECK(moq_msquic_conn_is_fatal(r->conn));
    CHECK(moq_transport_bridge_fatal_code(r->conn->bridge) == 0x1);
    CHECK(!enq0);
    CHECK(!obs0);
    CHECK(d0.total == 0);
    CHECK(moq_session_state(r->session) != MOQ_SESS_CLOSED);
    CHECK(!r->conn->close_pending);
    CHECK(!r->conn->close_fed);
    CHECK(r->conn->test_close_feed_commits == 0);
    CHECK(r->fake.conn_shutdowns == 1);
}

static void t_internal_fatal_then_transport_error(void)
{
    int before = failures;
    struct rig r;

    CHECK(rig_up(&r, NULL, NULL) == 0);
    printf("INTERNAL-FATAL-THEN-ERROR:\n");
    arm_internal_fatal(&r);

    /* A bare SHUTDOWN_COMPLETE is unclean and arrives after the callback that
     * caused ConnectionShutdown. MsQuic callbacks are serialized, never
     * recursive. */
    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    print_state("after-terminal", &r);

    struct drained d = drain_events(&r);

    printf("  events: closed=%d total=%d code=%llu\n", d.closed, d.total,
           (unsigned long long)d.close_code);
    check_told_once(&r, d, 0x1);
    CHECK(moq_msquic_conn_is_fatal(r.conn));
    CHECK(!moq_transport_bridge_is_closed(r.conn->bridge));
    CHECK(moq_transport_bridge_fatal_code(r.conn->bridge) == 0x1);
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: internal_fatal_then_transport_error\n");
}

static void t_internal_fatal_then_clean_close(void)
{
    int before = failures;
    struct rig r;
    QUIC_CONNECTION_EVENT ev;

    CHECK(rig_up(&r, NULL, NULL) == 0);
    printf("INTERNAL-FATAL-THEN-CLEAN-CLOSE:\n");
    arm_internal_fatal(&r);

    memset(&ev, 0, sizeof(ev));
    ev.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER;
    ev.SHUTDOWN_INITIATED_BY_PEER.ErrorCode = PEER_CLOSE_CODE;
    moq_msquic_conn_callback()(fake_msq_conn_handle(&r.fake), r.conn, &ev);
    print_state("after-peer-close", &r);

    struct drained d = drain_events(&r);

    /* The bridge was already fatal. The clean transport indication supplies
     * the missing session terminal but cannot replace the first cause or flip
     * the bridge to clean-closed. */
    check_told_once(&r, d, 0x1);
    CHECK(moq_msquic_conn_is_fatal(r.conn));
    CHECK(!moq_transport_bridge_is_closed(r.conn->bridge));
    CHECK(moq_transport_bridge_fatal_code(r.conn->bridge) == 0x1);

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: internal_fatal_then_clean_close\n");
}

/* --- 2. control: terminal BEFORE any service pass ------------------------ */

static void t_terminal_before_service(void)
{
    int before = failures;
    struct rig r;

    CHECK(rig_up(&r, NULL, NULL) == 0);
    printf("BEFORE-SERVICE:\n");

    /* nothing has serviced this connection yet */
    CHECK(!r.conn->close_pending);
    CHECK(!moq_msquic_conn_is_fatal(r.conn));

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    print_state("after-terminal", &r);

    struct drained d = drain_events(&r);

    check_told_once(&r, d, 0);
    /* flavor: an unsolicited completion is an unclean death, so the adapter
     * feeds it as a transport ERROR and the bridge latches fatal, not closed */
    CHECK(moq_msquic_conn_is_fatal(r.conn));
    CHECK(!moq_transport_bridge_is_closed(r.conn->bridge));
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: terminal_before_service\n");
}

/* --- 2b. control: a CLEAN peer close, the other flavor ------------------- */

static void t_clean_peer_close(void)
{
    int before = failures;
    struct rig r;
    QUIC_CONNECTION_EVENT ev;

    CHECK(rig_up(&r, NULL, NULL) == 0);
    printf("CLEAN-PEER-CLOSE:\n");

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_CONNECTED);
    (void)drain_events(&r);

    memset(&ev, 0, sizeof(ev));
    ev.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER;
    ev.SHUTDOWN_INITIATED_BY_PEER.ErrorCode = PEER_CLOSE_CODE;
    moq_msquic_conn_callback()(fake_msq_conn_handle(&r.fake), r.conn, &ev);
    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    print_state("after-peer-close", &r);

    struct drained d = drain_events(&r);

    /* flavor and code: the peer's orderly application close, carried through */
    check_told_once(&r, d, PEER_CLOSE_CODE);
    CHECK(moq_transport_bridge_is_closed(r.conn->bridge));
    CHECK(!moq_msquic_conn_is_fatal(r.conn));
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: clean_peer_close\n");
}

/* --- 3. control: terminal DURING an active service pass ------------------ */

/* Synthetic guard control: inject a callback while the application hook has a
 * service pass on the stack. Production MsQuic connection callbacks are
 * serialized and non-recursive; this pins the generic re-entrancy defense and
 * is not evidence for the already-fatal defect above. */
static int g_reentrant_hook_calls;
static bool g_reentrant_saw_in_service;
static bool g_reentrant_saw_unfed;

static void reentrant_hook(moq_msquic_conn_t *conn, void *user)
{
    struct rig *r = user;

    (void)conn;
    if (g_reentrant_hook_calls++ != 0)
        return;                  /* exactly one terminal, from the first pass */
    g_reentrant_saw_in_service = r->conn->in_service;
    deliver_conn_event(r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    /* the re-entrant service could not feed it: that is the active pass's job */
    g_reentrant_saw_unfed = r->conn->close_pending && !r->conn->close_fed;
}

static void t_terminal_during_service(void)
{
    int before = failures;
    struct rig r;

    g_reentrant_hook_calls = 0;
    g_reentrant_saw_in_service = false;
    g_reentrant_saw_unfed = false;
    CHECK(rig_up(&r, reentrant_hook, &r) == 0);
    printf("DURING-SERVICE:\n");

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_CONNECTED);
    print_state("after-connected", &r);

    struct drained d = drain_events(&r);

    CHECK(g_reentrant_hook_calls >= 1);
    CHECK(g_reentrant_saw_in_service);   /* the terminal really was in-pass */
    CHECK(g_reentrant_saw_unfed);        /* and the re-entrant call deferred */
    CHECK(!r.conn->in_service);          /* the pass has exited */
    check_told_once(&r, d, 0);
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: terminal_during_service\n");
}

/* --- 4. control: terminal AFTER a completed service pass ----------------- */

static void t_terminal_after_service(void)
{
    int before = failures;
    struct rig r;

    CHECK(rig_up(&r, NULL, NULL) == 0);
    printf("AFTER-SERVICE:\n");

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_CONNECTED);
    moq_msquic_conn_service(r.conn);
    /* a healthy connection: the control bidi opened and nothing is terminal */
    CHECK(!moq_msquic_conn_is_fatal(r.conn));
    CHECK(!r.conn->in_service);
    CHECK(fake_msq_stream_at(&r.fake, 0) != NULL);
    struct drained d0 = drain_events(&r);

    CHECK(d0.closed == 0);

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    print_state("after-terminal", &r);

    struct drained d = drain_events(&r);

    check_told_once(&r, d, 0);
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: terminal_after_service\n");
}

/* --- 5. control: duplicate completion ------------------------------------ */

static void t_duplicate_completion(void)
{
    int before = failures;
    struct rig r;

    CHECK(rig_up(&r, NULL, NULL) == 0);
    printf("DUPLICATE:\n");

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_CONNECTED);
    (void)drain_events(&r);
    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    print_state("after-3-terminals", &r);

    struct drained d = drain_events(&r);

    check_told_once(&r, d, 0);
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: duplicate_completion\n");
}

/* --- 6. control: the application stops the session ----------------------- */

/*
 * A local close is the session's own terminal: it queues SESSION_CLOSED and a
 * CLOSE_SESSION action the bridge turns into a transport shutdown. The
 * transport terminal that follows must not produce a second one.
 */
static void t_local_stop_then_terminal(void)
{
    int before = failures;
    struct rig r;

    CHECK(rig_up(&r, NULL, NULL) == 0);
    printf("LOCAL-STOP:\n");

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_CONNECTED);
    (void)drain_events(&r);

    CHECK(moq_session_close(r.session, LOCAL_CLOSE_CODE, NULL, 0) == MOQ_OK);
    moq_msquic_conn_service(r.conn);
    print_state("after-local-close", &r);

    struct drained d = drain_events(&r);

    /* the session's own terminal, carrying the application's code */
    CHECK(d.total == 1);
    CHECK(d.closed == 1);
    CHECK(d.close_code == LOCAL_CLOSE_CODE);
    CHECK(moq_session_state(r.session) == MOQ_SESS_CLOSED);
    CHECK(r.fake.conn_shutdowns >= 1);

    deliver_conn_event(&r, QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE);
    print_state("after-terminal", &r);

    struct drained d2 = drain_events(&r);

    CHECK(d2.total == 0);               /* exactly one terminal, total */
    CHECK(d2.closed == 0);
    CHECK(r.conn->close_fed);
    check_no_second_terminal(&r);

    rig_down(&r);
    if (failures == before)
        printf("PASS: local_stop_then_terminal\n");
}

int main(void)
{
    t_internal_fatal_then_transport_error();
    t_internal_fatal_then_clean_close();
    t_terminal_before_service();
    t_clean_peer_close();
    t_terminal_during_service();
    t_terminal_after_service();
    t_duplicate_completion();
    t_local_stop_then_terminal();

    if (failures == 0)
        printf("PASS: msquic_close_feed\n");
    else
        fprintf(stderr, "FAIL: msquic_close_feed (%d)\n", failures);
    return failures;
}
