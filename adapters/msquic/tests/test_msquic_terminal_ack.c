/*
 * Managed MsQuic terminal-child LIFETIME: a bounded application pump that
 * leaves MOQ_EVENT_SESSION_CLOSED unconsumed must still find its child, its
 * session and its per-connection state on a LATER pump.
 *
 * A server observes terminal by polling SESSION_CLOSED inside the lane pump,
 * and a BOUNDED consumer — one that returns from a pump before it has drained
 * that event — needs the child to survive until it has actually consumed the
 * terminal. It cannot detach late either: the header warns that session and
 * connection POINTER VALUES are not stable identities, so a child that stops
 * appearing from lane_next_conn simply vanishes from under whatever state the
 * application keyed on it. moq_msquic_managed_conn_ack_terminal is what closes
 * that: the child is reclaimed when the APPLICATION says so, not when a pump
 * opportunity has passed. This pins both halves — the retention before the
 * acknowledgment, and the exactly-once reclamation after it.
 *
 * This exercises the real production path — mgd_hook -> doorbell_main ->
 * doorbell_reap -> mgd_free_child — over real MsQuic loopback. It models no
 * state machine of its own.
 *
 * WHY IT IS DETERMINISTIC, phase by phase (no sleeps, no retry-until-lucky,
 * no timing assertion; the per-call waits are hang guards only):
 *
 *   1. ESTABLISH. The server pump drains events until it sees SETUP_COMPLETE.
 *   2. CLOSE. On that same pump it calls moq_msquic_managed_conn_close() once.
 *      The pump's own post-service feeds the close, so the session reaches
 *      MOQ_SESS_CLOSED and queues SESSION_CLOSED; SHUTDOWN_COMPLETE — the fact
 *      that makes the child reclaimable — arrives in a later worker batch.
 *   3. DECLINE. Every pump that finds a conn whose moq_session_state() is
 *      MOQ_SESS_CLOSED consumes NOTHING while the gate is shut, and records
 *      that it saw the terminal child. Reading the state is a non-consuming,
 *      public observation, and the lane lock is held for the whole pump, so
 *      the conn cannot change state underneath it. The pump never calls
 *      lane_wake: arming lane work would keep the child alive through the
 *      doorbell's work-pending branch and hide the defect.
 *   4. GATE. The gate opens on whichever of two ABSORBING doorbell states
 *      happens first, both observed on the doorbell thread itself:
 *        - a reclamation actually happened (the reap-gap seam fired), or
 *        - the doorbell reached its idle wait with the child still present AND
 *          fully quiesced (the reapable seam), i.e. it passed a reap decision
 *          point without reclaiming.
 *      Neither can be reached in the window where the session is closed but
 *      SHUTDOWN_COMPLETE has not arrived, so the application can never consume
 *      "early" and mask the defect, and no wall-clock ordering is involved.
 *   5. LATER PUMP. Only then does the test open the gate and ask for a pump.
 *      The contract owes that pump the same child, its session and the `user`
 *      state the application attached. It consumes the terminal, acknowledges,
 *      and the child is then reclaimed exactly once with its reserve released.
 *
 * ORACLE. Primary facts are direct and non-ASan: whether a child was reclaimed
 * while its terminal was unconsumed, whether the connection reserve was
 * released early, and whether the later pump was given the child and the
 * application's own `user` state back. The stale session pointer is never
 * dereferenced here — the use-after-free that follows from these facts is
 * corroborated by the Relay release-matrix ASan log, not re-executed.
 *
 * The acknowledgment's own results are checked where they occur: refused as
 * MOQ_ERR_WRONG_STATE on the declining pump (a completed transport shutdown is
 * not observation), MOQ_OK once the terminal has been polled, MOQ_OK again for
 * the documented harmless duplicate, MOQ_ERR_WRONG_STATE on the client
 * connection, and MOQ_ERR_INVAL for NULL. "Outside any callback" is
 * deliberately NOT exercised: obtaining a valid handle to try it with would
 * require exactly the cross-callback retention the contract forbids.
 *
 * The greedy control below runs the identical fixture with the gate open from
 * the start. It must pass: it proves the terminal event really is delivered on
 * a pump where the child is visible, so the bounded case's "unconsumed" is the
 * application's deliberate choice, not a missing event.
 *
 * Real MsQuic loopback. Compiled with MOQ_MSQUIC_TESTING (seams never ship).
 *
 * Usage: test_msquic_terminal_ack <cert.pem> <key.pem>
 */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <moq/msquic_managed.h>

#include <msquic.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* Bounded hang guards. Iterations, never an elapsed-time assertion. */
enum { SPIN_MAX = 40000, WAIT_US = 2 * 1000 };

#define CLOSE_CODE 0x11u
#define BIND_MAGIC 0x5ea15eeduL

/* --- white-box seams (MOQ_MSQUIC_TESTING build only) ---------------------- */
extern void (*moq_msq_test_reap_gap)(moq_msquic_managed_lane_t *lane);
extern void (*moq_msq_test_prewait)(moq_msquic_managed_lane_t *lane);
extern size_t moq_msq_test_lane_reapable(moq_msquic_managed_lane_t *lane);

/* The application's own per-connection state, published through the supported
 * `user` slot. It is the identity bridge across pumps — deliberately not a
 * retained session pointer. */
struct app_bind {
    unsigned long magic;
    int attached_on_pump;
};

struct fx {
    struct app_bind bind;

    atomic_int pumps;          /* on_lane_pump sweeps entered              */
    atomic_int setup_seen;     /* SETUP_COMPLETE consumed                  */
    atomic_int close_sent;     /* conn_close issued exactly once           */
    atomic_int bind_attached;  /* conn_set_user done                       */
    atomic_int terminal_pump;  /* pump index of the first terminal sighting */
    atomic_int declined;       /* terminal pumps that consumed nothing     */

    atomic_int gate_open;      /* the app is ready to consume the terminal */
    atomic_int gated_pumps;    /* pumps entered after the gate opened      */
    atomic_int gated_with_conn;/* ... that were given the child            */
    atomic_int user_returned;  /* ... that got the app's own bind back     */
    atomic_int closed_polled;  /* SESSION_CLOSED events consumed           */

    atomic_int ack_gate;       /* the app is ready to RELEASE the child    */

    /* moq_msquic_managed_conn_ack_terminal results, as they occur */
    atomic_int ack_unobserved; /* on the declining pump: terminal not polled */
    atomic_int ack_ok;         /* on the releasing pump, after consuming it  */
    atomic_int ack_dup;        /* immediately again, same callback           */

    /* The live handle the pump was presented, kept so the main thread can try
     * to acknowledge from OUTSIDE the pump. Retaining a handle past its
     * callback is exactly what the contract forbids applications to do; it is
     * safe HERE, and only here, because the child is unacknowledged and the
     * facade is running, so nothing may reclaim it — and the test never
     * dereferences it, it only hands it to the call that must refuse it. */
    moq_msquic_managed_conn_t *_Atomic held;

    atomic_int reaps;          /* reclamations observed at the reap seam   */
    atomic_int violation;      /* reclaimed with the terminal unconsumed   */
    atomic_int idle_after_terminal;   /* doorbell idle waits after terminal */
    atomic_int quiesced_retained;     /* idle wait WITH the child retained  */
};

/* No moq_result_t can be this, so an untouched slot is distinguishable from
 * every real answer. */
#define RC_UNSET 0x7fffffff

static void fx_init(struct fx *f)
{
    atomic_store(&f->terminal_pump, -1);
    atomic_store(&f->ack_unobserved, RC_UNSET);
    atomic_store(&f->ack_ok, RC_UNSET);
    atomic_store(&f->ack_dup, RC_UNSET);
}

/* Both hook globals are ATOMIC: the doorbell thread reads them, and the main
 * thread publishes them AFTER moq_msquic_managed_create has already spawned
 * that thread, so there is no happens-before edge to rely on. */
static struct fx *_Atomic g_fx;
/* The seams are global function pointers, and BOTH facades in this fixture run
 * doorbells that reach them. Every hook below is therefore scoped to the
 * server's lane, or a client reclamation would be counted as the server's. */
static moq_msquic_managed_lane_t *_Atomic g_srv_lane;

/* Reclamation instant: doorbell_reap has just run mgd_free_child on a victim.
 * If the application had already seen that child terminal in a pump and has not
 * consumed SESSION_CLOSED, its session was destroyed out from under it. */
static void reap_gap_hook(moq_msquic_managed_lane_t *lane)
{
    struct fx *f = atomic_load(&g_fx);

    if (f == NULL || lane != atomic_load(&g_srv_lane))
        return;
    atomic_fetch_add(&f->reaps, 1);
    if (atomic_load(&f->terminal_pump) >= 0 &&
        atomic_load(&f->closed_polled) == 0)
        atomic_store(&f->violation, 1);
}

/* The doorbell is about to sleep: it found no lane work and has already run a
 * reap pass. Seeing a fully quiesced child STILL PRESENT here means that pass
 * declined to reclaim it — the state the contract requires and the second of
 * the gate's two absorbing outcomes. */
static void prewait_hook(moq_msquic_managed_lane_t *lane)
{
    struct fx *f = atomic_load(&g_fx);

    if (f == NULL || lane != atomic_load(&g_srv_lane) ||
        atomic_load(&f->terminal_pump) < 0)
        return;
    atomic_fetch_add(&f->idle_after_terminal, 1);
    if (moq_msq_test_lane_reapable(lane) >= 1)
        atomic_store(&f->quiesced_retained, 1);
}

/* --- the pumps ------------------------------------------------------------ */

static int server_pump(moq_msquic_managed_t *m,
                       moq_msquic_managed_lane_t *lane, uint64_t now,
                       void *user)
{
    struct fx *f = user;
    moq_msquic_managed_conn_t *c = NULL;
    int idx = atomic_fetch_add(&f->pumps, 1) + 1;
    bool gated = atomic_load(&f->gate_open) != 0;

    (void)m;
    (void)now;
    if (gated)
        atomic_fetch_add(&f->gated_pumps, 1);

    while ((c = moq_msquic_lane_next_conn(lane, c)) != NULL) {
        moq_session_t *s = moq_msquic_managed_conn_session(c);
        moq_event_t ev;

        if (s == NULL)
            continue;
        if (moq_msquic_managed_conn_user(c) == NULL) {
            f->bind.magic = BIND_MAGIC;
            f->bind.attached_on_pump = idx;
            moq_msquic_managed_conn_set_user(c, &f->bind);
            atomic_store(&f->bind_attached, 1);
        }

        /* Non-consuming, public terminal observation. The lane lock is held
         * for the whole pump, so this cannot change underneath us. */
        if (moq_session_state(s) != MOQ_SESS_CLOSED) {
            while (moq_session_poll_events(s, &ev, 1) > 0) {
                if (ev.kind == MOQ_EVENT_SETUP_COMPLETE) {
                    atomic_fetch_add(&f->setup_seen, 1);
                    /* Terminal, initiated from inside the exclusive window.
                     * The final events arrive on a later pump. */
                    if (atomic_exchange(&f->close_sent, 1) == 0)
                        moq_msquic_managed_conn_close(c, CLOSE_CODE);
                }
                moq_event_cleanup(&ev);
            }
            continue;
        }

        if (atomic_load(&f->terminal_pump) < 0)
            atomic_store(&f->terminal_pump, idx);

        if (!gated) {
            /* The bounded pump: it has seen the terminal child and returns
             * WITHOUT consuming the final event. Acknowledging here must be
             * refused — a completed transport shutdown is not observation. */
            atomic_fetch_add(&f->declined, 1);
            if (atomic_load(&f->ack_unobserved) == RC_UNSET)
                atomic_store(&f->ack_unobserved,
                             moq_msquic_managed_conn_ack_terminal(c));
            continue;
        }

        /* The later pump the contract owes a bounded consumer. */
        atomic_fetch_add(&f->gated_with_conn, 1);
        {
            const struct app_bind *b = moq_msquic_managed_conn_user(c);

            if (b == &f->bind && b->magic == BIND_MAGIC)
                atomic_store(&f->user_returned, 1);
        }
        while (moq_session_poll_events(s, &ev, 1) > 0) {
            if (ev.kind == MOQ_EVENT_SESSION_CLOSED)
                atomic_fetch_add(&f->closed_polled, 1);
            moq_event_cleanup(&ev);
        }
        atomic_store(&f->held, c);
        /* Terminal consumed: release the child. Only now may it be
         * reclaimed. The duplicate right behind it is documented harmless. */
        if (atomic_load(&f->ack_gate) != 0 &&
            atomic_load(&f->closed_polled) > 0 &&
            atomic_load(&f->ack_ok) == RC_UNSET) {
            atomic_store(&f->ack_ok,
                         moq_msquic_managed_conn_ack_terminal(c));
            atomic_store(&f->ack_dup,
                         moq_msquic_managed_conn_ack_terminal(c));
        }
    }
    return 0;
}

static atomic_int g_ack_client = RC_UNSET;
static atomic_int g_cli_closed;

static int client_pump(moq_msquic_managed_t *m,
                       moq_msquic_managed_lane_t *lane, uint64_t now,
                       void *user)
{
    moq_session_t *s = moq_msquic_managed_session(m);
    moq_event_t ev;

    (void)now;
    (void)user;
    while (s != NULL && moq_session_poll_events(s, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SESSION_CLOSED)
            atomic_fetch_add(&g_cli_closed, 1);
        moq_event_cleanup(&ev);
    }
    /* The client's one connection is iterable on its own lane, so it CAN be
     * offered here — but it is not reclaimed per-child, so acknowledging it is
     * refused. Recorded only once this side has OBSERVED its own terminal, so
     * the refusal is attributable to the client rule and not to the
     * not-yet-observed rule that would refuse it anyway. */
    if (atomic_load(&g_cli_closed) > 0 &&
        atomic_load(&g_ack_client) == RC_UNSET) {
        moq_msquic_managed_conn_t *c = moq_msquic_lane_next_conn(lane, NULL);

        if (c != NULL)
            atomic_store(&g_ack_client,
                         moq_msquic_managed_conn_ack_terminal(c));
    }
    return 0;
}

/* --- fixture -------------------------------------------------------------- */

struct rig {
    moq_msquic_managed_t *srv;
    moq_msquic_managed_t *cli;
};

static bool rig_up(struct rig *r, struct fx *f, const char *cert,
                   const char *key)
{
    moq_msquic_managed_cfg_t scfg;

    memset(r, 0, sizeof(*r));
    moq_msquic_managed_cfg_init_sized(&scfg, sizeof(scfg));
    scfg.alloc = moq_alloc_default();
    scfg.perspective = MOQ_PERSPECTIVE_SERVER;
    scfg.host = "127.0.0.1";
    scfg.port = 0;
    scfg.cert_path = cert;
    scfg.key_path = key;
    scfg.max_events = 64;
    /* ONE connection on ONE lane: the reap pass has a single candidate, so
     * every observation below names that child unambiguously. */
    scfg.max_connections = 1;
    scfg.lane_count = 1;
    scfg.on_lane_pump = server_pump;
    scfg.on_lane_pump_user = f;

    CHECK(moq_msquic_managed_create(&scfg, &r->srv) == MOQ_OK);
    if (r->srv == NULL)
        return false;
    atomic_store(&g_srv_lane, moq_msquic_managed_lane(r->srv, 0));
    uint16_t port = moq_msquic_managed_port(r->srv);

    CHECK(port != 0);
    if (port == 0)
        return false;

    moq_msquic_managed_cfg_t ccfg;

    moq_msquic_managed_cfg_init_sized(&ccfg, sizeof(ccfg));
    ccfg.alloc = moq_alloc_default();
    ccfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    ccfg.host = "127.0.0.1";
    ccfg.port = port;
    ccfg.insecure_skip_verify = true;
    ccfg.max_events = 64;
    ccfg.on_lane_pump = client_pump;
    CHECK(moq_msquic_managed_create(&ccfg, &r->cli) == MOQ_OK);
    return r->cli != NULL;
}

static void rig_down(struct rig *r)
{
    atomic_store(&g_srv_lane, NULL);
    if (r->cli != NULL) {
        (void)moq_msquic_managed_stop(r->cli);
        moq_msquic_managed_destroy(r->cli);
        r->cli = NULL;
    }
    if (r->srv != NULL) {
        (void)moq_msquic_managed_stop(r->srv);
        moq_msquic_managed_destroy(r->srv);
        r->srv = NULL;
    }
}

/* Drive both facades until the server pump has consumed SETUP_COMPLETE (which
 * is also what issues the close). */
static bool rig_establish(struct rig *r, struct fx *f)
{
    for (int i = 0; i < SPIN_MAX; i++) {
        if (atomic_load(&f->setup_seen) >= 1)
            return true;
        (void)moq_msquic_managed_wake(r->srv);
        (void)moq_msquic_managed_wake(r->cli);
        (void)moq_msquic_managed_wait(r->srv, WAIT_US);
    }
    return false;
}

/* --- the RED -------------------------------------------------------------- */

static void t_bounded_pump_keeps_terminal_child(const char *cert,
                                                const char *key)
{
    int before = failures;
    struct fx f;
    struct rig r;

    memset(&f, 0, sizeof(f));
    fx_init(&f);
    atomic_store(&g_ack_client, RC_UNSET);
    atomic_store(&g_cli_closed, 0);
    moq_result_t ack_null = moq_msquic_managed_conn_ack_terminal(NULL);

    atomic_store(&f.gate_open, 0); /* the bounded consumer: shut to start */
    atomic_store(&g_fx, &f);
    moq_msq_test_reap_gap = reap_gap_hook;
    moq_msq_test_prewait = prewait_hook;

    if (!rig_up(&r, &f, cert, key)) {
        rig_down(&r);
        moq_msq_test_reap_gap = NULL;
        moq_msq_test_prewait = NULL;
        atomic_store(&g_fx, NULL);
        return;
    }

    bool up = rig_establish(&r, &f);

    /* The terminal pump: the application has seen a closed session on a
     * visible child and returned without consuming its final event. */
    bool terminal = false;

    for (int i = 0; i < SPIN_MAX && !terminal; i++) {
        (void)moq_msquic_managed_wake(r.srv);
        (void)moq_msquic_managed_wake(r.cli);
        (void)moq_msquic_managed_wait(r.srv, WAIT_US);
        terminal = atomic_load(&f.terminal_pump) >= 0;
    }

    /* Let the doorbell reach a decision. Deliberately NO wakes here: the two
     * outcomes below are reached by the doorbell's own sequencing, and waking
     * it would let a pump run before the reap pass. */
    bool decided = false;

    for (int i = 0; i < SPIN_MAX && !decided; i++) {
        (void)moq_msquic_managed_wait(r.srv, WAIT_US);
        decided = atomic_load(&f.reaps) >= 1 ||
                  atomic_load(&f.quiesced_retained) != 0;
    }

    /* What the application would find when it comes back for its child. */
    size_t conns_at_gate = moq_msquic_managed_conn_count(r.srv);
    int reaped_before_gate = atomic_load(&f.reaps);

    atomic_store(&f.gate_open, 1);

    /* The later pump: it CONSUMES the terminal but does not release yet. */
    bool later = false;

    for (int i = 0; i < SPIN_MAX && !later; i++) {
        (void)moq_msquic_managed_wake(r.srv);
        (void)moq_msquic_managed_wait(r.srv, WAIT_US);
        later = atomic_load(&f.gated_pumps) >= 1 &&
                atomic_load(&f.closed_polled) >= 1;
    }

    /* OUTSIDE the pump. The child is live and unreclaimable (unacknowledged,
     * facade running) and its terminal HAS been observed, so the only rule
     * left to refuse this is the owning-lane-pump window itself — the flag
     * lives in that lock domain and an external writer would race the
     * doorbell's read. mgd_tls_lane is thread-local, so this thread is
     * outside every pump by construction, with no ordering to arrange. */
    moq_msquic_managed_conn_t *held = atomic_load(&f.held);
    moq_result_t ack_outside = RC_UNSET;
    int reaps_before_outside = atomic_load(&f.reaps);

    int idle_before_outside = atomic_load(&f.idle_after_terminal);

    if (held != NULL && reaps_before_outside == 0)
        ack_outside = moq_msquic_managed_conn_ack_terminal(held);
    /* Give the doorbell a full reap decision AFTER that call before sampling,
     * so "nothing was released" is a real observation rather than one taken
     * too early to mean anything. The doorbell reaches its idle wait on its
     * own; no wake is needed, and none is sent. */
    for (int i = 0; i < SPIN_MAX; i++) {
        if (atomic_load(&f.idle_after_terminal) > idle_before_outside)
            break;
        (void)moq_msquic_managed_wait(r.srv, WAIT_US);
    }
    size_t conns_after_outside = moq_msquic_managed_conn_count(r.srv);
    int reaps_after_outside = atomic_load(&f.reaps);

    /* The documented cost of holding on: the child still occupies one of
     * cfg.max_connections (which is 1 here), so a second client is refused at
     * the listener. Waiting on the refused client's own terminal is a positive
     * observation, not a timeout. */
    moq_msquic_managed_t *cli2 = NULL;
    moq_msquic_managed_cfg_t c2;

    moq_msquic_managed_cfg_init_sized(&c2, sizeof(c2));
    c2.alloc = moq_alloc_default();
    c2.perspective = MOQ_PERSPECTIVE_CLIENT;
    c2.host = "127.0.0.1";
    c2.port = moq_msquic_managed_port(r.srv);
    c2.insecure_skip_verify = true;
    c2.max_events = 64;
    c2.on_lane_pump = client_pump;
    bool cli2_up = moq_msquic_managed_create(&c2, &cli2) == MOQ_OK;
    bool cli2_refused = false;

    for (int i = 0; i < SPIN_MAX && cli2_up && !cli2_refused; i++) {
        (void)moq_msquic_managed_wake(cli2);
        (void)moq_msquic_managed_wait(cli2, WAIT_US);
        cli2_refused = moq_msquic_managed_is_fatal(cli2) ||
                       moq_msquic_managed_is_closed(cli2);
    }
    int setup_after_refuse = atomic_load(&f.setup_seen);
    size_t conns_after_refuse = moq_msquic_managed_conn_count(r.srv);

    if (cli2 != NULL) {
        (void)moq_msquic_managed_stop(cli2);
        moq_msquic_managed_destroy(cli2);
    }

    /* Now release it properly, from inside the owning lane's pump. */
    atomic_store(&f.ack_gate, 1);
    for (int i = 0; i < SPIN_MAX &&
                    atomic_load(&f.ack_ok) == RC_UNSET; i++) {
        (void)moq_msquic_managed_wake(r.srv);
        (void)moq_msquic_managed_wait(r.srv, WAIT_US);
    }

    /* Acknowledged: the child may now be reclaimed, and the reserve released.
     * The doorbell does that on its own next pass — no wake is needed. */
    bool released = false;

    for (int i = 0; i < SPIN_MAX && !released; i++) {
        (void)moq_msquic_managed_wait(r.srv, WAIT_US);
        released = atomic_load(&f.reaps) >= 1 &&
                   moq_msquic_managed_conn_count(r.srv) == 0;
    }
    size_t conns_after_ack = moq_msquic_managed_conn_count(r.srv);
    int reaps_after_ack = atomic_load(&f.reaps);

    /* The client side: it observes its own terminal and is reclaimed WITHOUT
     * any acknowledgment — the exemption in action. */
    bool cli_done = false;

    for (int i = 0; i < SPIN_MAX && !cli_done; i++) {
        (void)moq_msquic_managed_wake(r.cli);
        (void)moq_msquic_managed_wait(r.cli, WAIT_US);
        cli_done = atomic_load(&g_cli_closed) >= 1 &&
                   moq_msquic_managed_conn_count(r.cli) == 0;
    }
    size_t cli_conns_end = moq_msquic_managed_conn_count(r.cli);

    bool srv_fatal = moq_msquic_managed_is_fatal(r.srv);

    rig_down(&r);
    moq_msq_test_reap_gap = NULL;
    moq_msq_test_prewait = NULL;
    atomic_store(&g_fx, NULL);

    printf("TERMINAL-ACK bounded: pumps=%d setup=%d terminal_pump=%d "
           "declined=%d idle_after_terminal=%d quiesced_retained=%d "
           "reaps_before_gate=%d conns_at_gate=%zu gated_pumps=%d "
           "gated_with_conn=%d user_returned=%d closed_polled=%d "
           "violation=%d ack_unobserved=%d ack_ok=%d ack_dup=%d "
           "ack_null=%d ack_client=%d reaps_after_ack=%d "
           "conns_after_ack=%zu\n",
           atomic_load(&f.pumps), atomic_load(&f.setup_seen),
           atomic_load(&f.terminal_pump), atomic_load(&f.declined),
           atomic_load(&f.idle_after_terminal),
           atomic_load(&f.quiesced_retained), reaped_before_gate,
           conns_at_gate, atomic_load(&f.gated_pumps),
           atomic_load(&f.gated_with_conn), atomic_load(&f.user_returned),
           atomic_load(&f.closed_polled), atomic_load(&f.violation),
           atomic_load(&f.ack_unobserved), atomic_load(&f.ack_ok),
           atomic_load(&f.ack_dup), ack_null, atomic_load(&g_ack_client),
           reaps_after_ack, conns_after_ack);
    printf("TERMINAL-ACK outside-pump: ack=%d reaps=%d->%d conns=%zu\n",
           ack_outside, reaps_before_outside, reaps_after_outside,
           conns_after_outside);
    printf("TERMINAL-ACK capacity: second_client_created=%d refused=%d "
           "setup_seen=%d conns=%zu\n",
           (int)cli2_up, (int)cli2_refused, setup_after_refuse,
           conns_after_refuse);
    printf("TERMINAL-ACK client: closed_polled=%d conns_end=%zu ack=%d\n",
           atomic_load(&g_cli_closed), cli_conns_end,
           atomic_load(&g_ack_client));

    /* Fixture facts: the rig really reached the state under test. */
    CHECK(up);
    CHECK(terminal);
    CHECK(decided);
    CHECK(later);
    CHECK(atomic_load(&f.setup_seen) == 1);
    CHECK(atomic_load(&f.bind_attached) == 1);
    CHECK(atomic_load(&f.terminal_pump) > 0);
    CHECK(atomic_load(&f.declined) >= 1);
    CHECK(!srv_fatal);

    /* The lifetime contract. */
    /* 1. No child may be reclaimed while its terminal is unconsumed. */
    CHECK(atomic_load(&f.violation) == 0);
    /* 2. Nothing was reclaimed at all before the application was ready ... */
    CHECK(reaped_before_gate == 0);
    /* 3. ... the doorbell instead passed a reap decision point with the child
     *    transport-quiesced and deliberately retained ... */
    CHECK(atomic_load(&f.quiesced_retained) == 1);
    /* 4. ... and the connection reserve was still held. */
    CHECK(conns_at_gate == 1);
    /* 5. The later pump is given the same child ... */
    CHECK(atomic_load(&f.gated_with_conn) >= 1);
    /* 6. ... carrying the application's own per-connection state ... */
    CHECK(atomic_load(&f.user_returned) == 1);
    /* 7. ... and the final event is still there to consume, exactly once. */
    CHECK(atomic_load(&f.closed_polled) == 1);
    /* 8. Acknowledging is refused until the terminal has been OBSERVED: a
     *    completed transport shutdown is not enough. */
    CHECK(atomic_load(&f.ack_unobserved) == MOQ_ERR_WRONG_STATE);
    /* 9. Once observed it is accepted, and a duplicate is harmless. */
    CHECK(atomic_load(&f.ack_ok) == MOQ_OK);
    CHECK(atomic_load(&f.ack_dup) == MOQ_OK);
    /* 10. NULL is invalid input; and the client connection is refused even
     *     though that side HAS observed its own terminal — the refusal is the
     *     client rule, not the not-yet-observed rule. */
    CHECK(ack_null == MOQ_ERR_INVAL);
    CHECK(atomic_load(&g_cli_closed) >= 1);
    CHECK(atomic_load(&g_ack_client) == MOQ_ERR_WRONG_STATE);
    /* 11. ... and it is reclaimed anyway: the client is exempt. */
    CHECK(cli_done);
    CHECK(cli_conns_end == 0);
    /* 12. Acknowledging from OUTSIDE the owning lane's pump is refused, and
     *     refused inertly: nothing is released by it. */
    CHECK(ack_outside == MOQ_ERR_WRONG_STATE);
    CHECK(reaps_after_outside == 0);
    CHECK(conns_after_outside == 1);
    /* 13. The retained child costs an admission slot: a second client is
     *     refused, and the server never accepts it. */
    CHECK(cli2_up);
    CHECK(cli2_refused);
    CHECK(setup_after_refuse == 1);
    CHECK(conns_after_refuse == 1);
    /* 14. Having acknowledged from inside the pump, the server child is
     *     reclaimed EXACTLY once and its admission slot comes back. */
    CHECK(released);
    CHECK(reaps_after_ack == 1);
    CHECK(conns_after_ack == 0);

    if (failures == before)
        printf("PASS: bounded_pump_keeps_terminal_child\n");
}

/* --- the greedy control (must pass today) --------------------------------- */

static void t_greedy_pump_consumes_terminal(const char *cert, const char *key)
{
    int before = failures;
    struct fx f;
    struct rig r;

    memset(&f, 0, sizeof(f));
    fx_init(&f);
    atomic_store(&g_ack_client, RC_UNSET);
    atomic_store(&g_cli_closed, 0);
    atomic_store(&f.gate_open, 1); /* consume on the terminal pump itself */
    atomic_store(&f.ack_gate, 1);  /* ... and release it in the same pump   */
    atomic_store(&g_fx, &f);
    moq_msq_test_reap_gap = reap_gap_hook;
    moq_msq_test_prewait = prewait_hook;

    if (!rig_up(&r, &f, cert, key)) {
        rig_down(&r);
        moq_msq_test_reap_gap = NULL;
        moq_msq_test_prewait = NULL;
        atomic_store(&g_fx, NULL);
        return;
    }

    bool up = rig_establish(&r, &f);
    bool done = false;

    for (int i = 0; i < SPIN_MAX && !done; i++) {
        (void)moq_msquic_managed_wake(r.srv);
        (void)moq_msquic_managed_wake(r.cli);
        (void)moq_msquic_managed_wait(r.srv, WAIT_US);
        done = atomic_load(&f.closed_polled) >= 1 &&
               moq_msquic_managed_conn_count(r.srv) == 0;
    }

    size_t conns_end = moq_msquic_managed_conn_count(r.srv);
    bool srv_fatal = moq_msquic_managed_is_fatal(r.srv);

    rig_down(&r);
    moq_msq_test_reap_gap = NULL;
    moq_msq_test_prewait = NULL;
    atomic_store(&g_fx, NULL);

    printf("TERMINAL-ACK greedy: pumps=%d setup=%d terminal_pump=%d "
           "gated_with_conn=%d user_returned=%d closed_polled=%d reaps=%d "
           "violation=%d conns_end=%zu ack_ok=%d ack_dup=%d\n",
           atomic_load(&f.pumps), atomic_load(&f.setup_seen),
           atomic_load(&f.terminal_pump), atomic_load(&f.gated_with_conn),
           atomic_load(&f.user_returned), atomic_load(&f.closed_polled),
           atomic_load(&f.reaps), atomic_load(&f.violation), conns_end,
           atomic_load(&f.ack_ok), atomic_load(&f.ack_dup));

    CHECK(up);
    CHECK(done);
    CHECK(atomic_load(&f.setup_seen) == 1);
    /* The final event IS delivered on a pump where the child is visible, and
     * the application's own state comes back with it. */
    CHECK(atomic_load(&f.gated_with_conn) >= 1);
    CHECK(atomic_load(&f.user_returned) == 1);
    CHECK(atomic_load(&f.closed_polled) == 1);
    /* Having consumed AND acknowledged it, the child is reclaimed exactly
     * once and its reserve released. */
    CHECK(atomic_load(&f.ack_ok) == MOQ_OK);
    CHECK(atomic_load(&f.ack_dup) == MOQ_OK);
    CHECK(atomic_load(&f.reaps) == 1);
    CHECK(conns_end == 0);
    CHECK(atomic_load(&f.violation) == 0);
    CHECK(!srv_fatal);

    if (failures == before)
        printf("PASS: greedy_pump_consumes_terminal\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }
    static const QUIC_API_TABLE *lib_pin;

    if (QUIC_FAILED(MsQuicOpen2(&lib_pin))) {
        fprintf(stderr, "MsQuicOpen2 failed\n");
        return 2;
    }

    t_greedy_pump_consumes_terminal(argv[1], argv[2]);
    t_bounded_pump_keeps_terminal_child(argv[1], argv[2]);

    if (failures == 0)
        printf("PASS: msquic_terminal_ack\n");
    else
        fprintf(stderr, "FAIL: msquic_terminal_ack (%d)\n", failures);
    return failures;
}
