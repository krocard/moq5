/*
 * moq_endpoint_t lifecycle over the managed picoquic facades. A
 * threaded-picoquic SERVER (direct facade use -- the endpoint is
 * client-only in v0) loops back a real QUIC connection to the endpoint
 * client: establish, wait semantics (wake/timeout/terminal), the sticky
 * interrupt latch (including interrupt-while-blocked), the post() executor
 * (FIFO, exactly-once, post-after-terminal, NULL-session terminal drain),
 * and stop/destroy ordering + idempotency.
 */
#include <moq/endpoint.h>
#include <moq/media_receiver.h>
#include <moq/media_sender.h>
#include <moq/picoquic_threaded.h>
#include <moq/session.h>   /* moq_session_version: the endpoint/session agreement */
#include "test_support.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sched.h>       /* sched_yield (wake-unblock waiter handoff) */
#include <stdint.h>      /* UINT64_MAX (indefinite wait) */

static int failures = 0;

/* -- server side (direct facade) ------------------------------------ */

static int server_pump(moq_pq_threaded_t *t, moq_pq_threaded_lane_t *lane,
                       uint64_t now_us, void *ctx)
{
    (void)t; (void)lane; (void)now_us; (void)ctx;
    return 0;
}
static moq_pq_threaded_t *start_server_ex(const char *cert, const char *key,
                                          const char *const *alpn_list,
                                          size_t alpn_count, int *out_port)
{
    /* Try a few ports; bind collisions retry. Each call shifts the base:
     * the v0 server accepts ONE connection ever, so every scenario starts a
     * fresh server and must not collide with an earlier one's port. */
    static int calls = 0;
    int base = 14400 + (int)(getpid() % 997) + (calls++ * 131);
    for (int attempt = 0; attempt < 8; attempt++) {
        int port = base + attempt * 13;
        moq_pq_threaded_cfg_t cfg;
        moq_pq_threaded_cfg_init(&cfg);
        cfg.alloc = moq_alloc_default();
        cfg.perspective = MOQ_PERSPECTIVE_SERVER;
        cfg.cert_path = cert;
        cfg.key_path = key;
        cfg.port = port;
        cfg.send_request_capacity = true;
        cfg.initial_request_capacity = 16;
        cfg.on_lane_pump = server_pump;
        cfg.alpn_list = alpn_list;
        cfg.alpn_count = alpn_count;
        moq_pq_threaded_t *srv = NULL;
        if (moq_pq_threaded_create(&cfg, &srv) == MOQ_OK) {
            *out_port = port;
            return srv;
        }
    }
    return NULL;
}

static moq_pq_threaded_t *start_server(const char *cert, const char *key,
                                       int *out_port)
{
    return start_server_ex(cert, key, NULL, 0, out_port);
}

/* -- helpers ---------------------------------------------------------- */

static moq_endpoint_cfg_t client_cfg(char *urlbuf, size_t cap, int port)
{
    moq_endpoint_cfg_t c;
    moq_endpoint_cfg_init(&c);
    snprintf(urlbuf, cap, "moqt://127.0.0.1:%d", port);
    c.url = (moq_bytes_t){ (const uint8_t *)urlbuf, strlen(urlbuf) };
    c.insecure_skip_verify = true;       /* self-signed loopback cert */
    /* Offer defaults to AUTO: {18, 16}, newest first. */
    return c;
}

/* Wait (bounded) until the endpoint reaches `want`; true on success. */
static bool wait_for_state(moq_endpoint_t *ep, moq_endpoint_state_t want,
                           int budget_ms)
{
    int waited = 0;
    while (waited < budget_ms) {
        if (moq_endpoint_state(ep) == want) return true;
        moq_result_t rc = moq_endpoint_wait(ep, 100000);
        if (rc == MOQ_ERR_CLOSED && want != MOQ_ENDPOINT_CLOSED) return false;
        waited += 100;
    }
    return moq_endpoint_state(ep) == want;
}

/* -- wake-unblock plumbing --------------------------------------------- */

typedef struct {
    moq_endpoint_t *ep;
    atomic_int      entered;   /* waiter is about to block (or already did) */
    moq_result_t    rc;
} wake_waiter_t;

static void *wake_waiter_fn(void *arg)
{
    wake_waiter_t *w = (wake_waiter_t *)arg;
    atomic_store(&w->entered, 1);
    w->rc = moq_endpoint_wait(w->ep, UINT64_MAX);   /* indefinite */
    return NULL;
}

/* -- post() task plumbing --------------------------------------------- */

/*
 * LIFETIME, PUBLICATION AND EXACTLY-ONCE, all required by endpoint.h
 * section 5.4.
 *
 * Lifetime: an accepted post() task runs EXACTLY ONCE, but not necessarily
 * before the poster stops waiting -- on a timeout it runs later, during the
 * stop()/destroy() terminal drain. A block-local context would then be written
 * through after its scope ended. Every context below, and every object one
 * points at, therefore has STATIC storage: it stays alive through stop(),
 * destroy() and until process termination, so a late task can never write
 * through a dangling pointer.
 *
 * Publication: the task writes its result fields FIRST and publishes
 * completion LAST with a release RMW on `done`; a reader acquires `done`
 * before observing any result.
 *
 * Exactly-once: the results are ATOMIC, not plain. The test observes them
 * while the endpoint is still live, so if the contract under test is BROKEN
 * and the task runs a second time, that invocation writes concurrently with
 * the reader. Plain fields would make this fixture race under the very defect
 * it exists to diagnose; atomic fields make the violation defined and
 * diagnosable instead. The counts are additionally reasserted after
 * moq_endpoint_stop() has drained all accepted work, so a duplicate that
 * happens LATER in the endpoint's lifetime cannot slip past a check taken at
 * the first completion.
 *
 * Because the statics are reused across cases, each is reset before use.
 */
typedef struct {
    /* inputs */
    atomic_int *order_ctr;      /* shared FIFO counter (static storage) */
    int         id;

    /* results -- atomic; written before `done` is published */
    atomic_int  order_seen;     /* this task's draw from order_ctr; -1 = none */
    atomic_int  saw_null_session;
    atomic_int  saw_live_session;

    /* completion -- published LAST, with release */
    atomic_int  done;           /* invocation count */
} task_ctx_t;

/* Static storage: see the LIFETIME note above. */
static atomic_int  g_task_order;
static task_ctx_t  g_task_a, g_task_b, g_task_after_stop, g_task_wt;

static void task_ctx_reset(task_ctx_t *t, atomic_int *order_ctr, int id)
{
    t->order_ctr = order_ctr;
    t->id = id;
    atomic_init(&t->order_seen, -1);
    atomic_init(&t->saw_null_session, 0);
    atomic_init(&t->saw_live_session, 0);
    atomic_init(&t->done, 0);
}

/* Acquire completion; returns the invocation count. A caller observes the
 * result fields only after this returns >= 1. */
static int task_ctx_done(const task_ctx_t *t)
{
    return atomic_load_explicit(&((task_ctx_t *)t)->done, memory_order_acquire);
}

static int task_ctx_get(const atomic_int *f)
{
    return atomic_load_explicit((atomic_int *)f, memory_order_relaxed);
}

static moq_result_t task_fn(moq_endpoint_t *ep, moq_session_t *session,
                            uint64_t now_us, void *ctx)
{
    (void)ep; (void)now_us;
    task_ctx_t *t = (task_ctx_t *)ctx;

    /* results first ... */
    if (t->order_ctr)
        atomic_store_explicit(&t->order_seen,
                              atomic_fetch_add(t->order_ctr, 1),
                              memory_order_relaxed);
    if (session)
        atomic_store_explicit(&t->saw_live_session, 1, memory_order_relaxed);
    else
        atomic_store_explicit(&t->saw_null_session, 1, memory_order_relaxed);

    /* ... completion last: release, so an acquiring reader sees the above */
    atomic_fetch_add_explicit(&t->done, 1, memory_order_release);
    return MOQ_OK;
}

/* The full FIFO postcondition, asserted both at first completion and again
 * after the terminal drain. `expect_a`/`expect_b` pin the order draws so a
 * late duplicate that redraws the shared counter is caught, not just a
 * duplicate that bumps the count. */
static void check_fifo_post(const char *when, int expect_a, int expect_b)
{
    int da = task_ctx_done(&g_task_a);
    int db = task_ctx_done(&g_task_b);

    if (da != 1 || db != 1)
        fprintf(stderr, "FAIL[fifo.%s]: done a=%d b=%d (want 1,1)\n",
                when, da, db);
    MOQ_TEST_CHECK_EQ_INT(da, 1);              /* exactly once */
    MOQ_TEST_CHECK_EQ_INT(db, 1);
    if (da >= 1 && db >= 1) {
        int oa = task_ctx_get(&g_task_a.order_seen);
        int ob = task_ctx_get(&g_task_b.order_seen);
        MOQ_TEST_CHECK(oa < ob);                        /* FIFO */
        if (expect_a >= 0) MOQ_TEST_CHECK_EQ_INT(oa, expect_a);   /* stable */
        if (expect_b >= 0) MOQ_TEST_CHECK_EQ_INT(ob, expect_b);
        MOQ_TEST_CHECK_EQ_INT(task_ctx_get(&g_task_a.saw_live_session), 1);
        MOQ_TEST_CHECK_EQ_INT(task_ctx_get(&g_task_b.saw_live_session), 1);
        MOQ_TEST_CHECK_EQ_INT(task_ctx_get(&g_task_a.saw_null_session), 0);
        MOQ_TEST_CHECK_EQ_INT(task_ctx_get(&g_task_b.saw_null_session), 0);
    }
}

/* -- endpoint/session version agreement -------------------------------- *
 * The one point where both facts are legitimately observable at once: a
 * post() task runs on the managed execution context and is handed BOTH the
 * endpoint and its live session (endpoint.h section 5.4), so it can read the
 * transport's negotiated version and the session's own wire profile without
 * any test-only accessor or internal header. The task may legitimately run
 * with session == NULL during terminal drain; that is recorded, never
 * dereferenced.
 *
 * LIFETIME: an accepted post() task is guaranteed to run EXACTLY ONCE, but
 * not necessarily before the poster stops waiting -- on the timeout path it
 * runs later, during the terminal drain. Block-local context would then be
 * written through after its scope ended, turning a test FAILURE into a
 * delayed stack use-after-return. The context therefore has STATIC storage:
 * it stays alive through the endpoint, stop(), destroy() and until process
 * termination, so the task can never write through a dangling pointer no
 * matter when it runs. Synchronization is unchanged -- the reader acquires on `ran`, which
 * the task releases after publishing both versions -- and nothing sleeps. */
typedef struct {
    /* Result fields are ATOMIC, not plain: on the timeout path the reader
     * leaves the bounded wait with ran == 0, records the failure, and the
     * accepted task may still be writing. Atomic storage makes that read
     * defined (it reports the stale zero the failing assertion wants to show)
     * instead of a data race. Publication order is still results-then-`ran`
     * with release, so a successful reader sees a coherent pair. */
    atomic_int ran;
    atomic_int saw_null_session;
    atomic_int ep_version;
    atomic_int session_version;
} version_probe_t;

/* Static storage duration: see the LIFETIME note above. */
static version_probe_t g_version_probe;

static moq_result_t version_probe_fn(moq_endpoint_t *ep,
                                     moq_session_t *session,
                                     uint64_t now_us, void *ctx)
{
    (void)now_us;
    version_probe_t *p = (version_probe_t *)ctx;

    if (!session) {
        atomic_store_explicit(&p->saw_null_session, 1, memory_order_relaxed);
    } else {
        atomic_store_explicit(&p->ep_version,
                              (int)moq_endpoint_negotiated_version(ep),
                              memory_order_relaxed);
        atomic_store_explicit(&p->session_version,
                              (int)moq_session_version(session),
                              memory_order_relaxed);
    }
    /* release: publishes both versions to whoever acquires on `ran` */
    atomic_fetch_add_explicit(&p->ran, 1, memory_order_release);
    return MOQ_OK;
}

/* -- interrupt-while-blocked helper thread ----------------------------- */

typedef struct {
    moq_endpoint_t *ep;
    unsigned        delay_ms;
} latch_arg_t;

static void *latch_setter(void *arg)
{
    latch_arg_t *la = (latch_arg_t *)arg;
    usleep(la->delay_ms * 1000);
    moq_endpoint_set_interrupted(la->ep, true);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *cert = NULL, *key = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cert") && i + 1 < argc) cert = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
    }
    if (!cert || !key) {
        fprintf(stderr, "usage: --cert <pem> --key <pem>\n");
        return 1;
    }

    int port = 0;
    moq_pq_threaded_t *srv = start_server(cert, key, &port);
    MOQ_TEST_CHECK(srv != NULL);
    if (!srv) return 1;

    /* == Malformed CA file -> MOQ_ERR_INVAL (preflighted config error) === *
     * A CA bundle that cannot be loaded is a configuration error surfaced by
     * connect() up front, not a generic transport/handshake failure. */
    {
        char ca_path[] = "/tmp/moq_bad_ca_XXXXXX";
        int fd = mkstemp(ca_path);
        MOQ_TEST_CHECK(fd >= 0);
        if (fd >= 0) {
            static const char junk[] =
                "-----BEGIN CERTIFICATE-----\nnotbase64\n"
                "-----END CERTIFICATE-----\n";
            (void)write(fd, junk, sizeof(junk) - 1);
            close(fd);
            char bad_url[64];
            snprintf(bad_url, sizeof(bad_url), "moqt://127.0.0.1:%d", port);
            moq_endpoint_cfg_t bc;
            moq_endpoint_cfg_init(&bc);
            bc.url = (moq_bytes_t){ (const uint8_t *)bad_url, strlen(bad_url) };
            bc.insecure_skip_verify = false;
            bc.ca_file = (moq_bytes_t){ (const uint8_t *)ca_path,
                                        strlen(ca_path) };
            moq_endpoint_t *bad_ep = NULL;
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&bc, &bad_ep),
                                  (int)MOQ_ERR_INVAL);
            MOQ_TEST_CHECK(bad_ep == NULL);
            unlink(ca_path);
        }
    }

    /* == Loopback: connect -> ESTABLISHED, negotiated version ========= */
    char url[64];
    moq_endpoint_cfg_t c = client_cfg(url, sizeof(url), port);
    moq_endpoint_t *ep = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&c, &ep), (int)MOQ_OK);
    MOQ_TEST_CHECK(ep != NULL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(ep), 0);
    MOQ_TEST_CHECK(wait_for_state(ep, MOQ_ENDPOINT_ESTABLISHED, 15000));
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(ep),
                          (int)MOQ_VERSION_DRAFT_16);
    MOQ_TEST_CHECK(!moq_endpoint_is_fatal(ep));
    MOQ_TEST_CHECK(!moq_endpoint_is_closed(ep));

    /* == the endpoint's negotiated version IS the session's profile ==== *
     * Established and not closing, so the task runs with a live session.
     * Both values are read inside the same task invocation, on the context
     * that owns them -- not sampled from two different threads at two
     * different times. */
    {
        version_probe_t *vp = &g_version_probe;   /* static: see LIFETIME */
        atomic_init(&vp->ran, 0);
        atomic_init(&vp->saw_null_session, 0);
        atomic_init(&vp->ep_version, 0);
        atomic_init(&vp->session_version, 0);

        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_post(ep, version_probe_fn, vp),
                              (int)MOQ_OK);
        int waited = 0;
        /* acquire on `ran`; no sleep, the wait is the endpoint's own */
        while (atomic_load_explicit(&vp->ran, memory_order_acquire) == 0 &&
               waited < 5000) {
            (void)moq_endpoint_wait(ep, 100000);
            waited += 100;
        }
        int vran = atomic_load_explicit(&vp->ran, memory_order_acquire);
        MOQ_TEST_CHECK_EQ_INT(vran, 1);
        MOQ_TEST_CHECK_EQ_INT(
            atomic_load_explicit(&vp->saw_null_session, memory_order_relaxed),
            0);
        int sv = atomic_load_explicit(&vp->session_version,
                                      memory_order_relaxed);
        int ev = atomic_load_explicit(&vp->ep_version, memory_order_relaxed);
        MOQ_TEST_CHECK_EQ_INT(sv, (int)MOQ_VERSION_DRAFT_16);
        MOQ_TEST_CHECK_EQ_INT(ev, sv);
    }

    /* == wait: timeout (MOQ_DONE) is observable once quiescent ======== */
    {
        bool saw_done = false;
        for (int i = 0; i < 50 && !saw_done; i++)
            saw_done = (moq_endpoint_wait(ep, 50000) == MOQ_DONE);
        MOQ_TEST_CHECK(saw_done);
    }

    /* == wake: the public pump nudge unblocks an INDEFINITE wait ======= */
    {
        /* A waiter blocks with NO timeout; the public moq_endpoint_wake()
         * must let it return. Progress-only: the join is the assertion (a
         * lost wake hangs here and fails via the ctest timeout — no latency
         * is measured). The activity flag is level-retained, so BOTH
         * interleavings are correct: wake before the waiter blocks makes
         * the wait return immediately; wake after breaks the block. */
        wake_waiter_t w;
        w.ep = ep;
        atomic_init(&w.entered, 0);
        w.rc = MOQ_ERR_INTERNAL;
        pthread_t th;
        MOQ_TEST_CHECK_EQ_INT(pthread_create(&th, NULL, wake_waiter_fn, &w), 0);
        while (!atomic_load(&w.entered)) sched_yield();
        moq_endpoint_wake(ep);
        pthread_join(th, NULL);
        MOQ_TEST_CHECK_EQ_INT((int)w.rc, (int)MOQ_OK);
    }

    /* == drain: an established endpoint that has written no stream data has
     *    nothing to flush, so the public wrapper drains immediately through
     *    the real (picoquic) backend vtable and returns MOQ_OK. =========== */
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_drain(ep, 2000000), (int)MOQ_OK);

    /* == Sticky interrupt latch ======================================== */
    {
        /* Latched: blocking calls return INTERRUPTED immediately,
         * including repeated calls (no re-block race). */
        moq_endpoint_set_interrupted(ep, true);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_wait(ep, 5000000),
                              (int)MOQ_ERR_INTERRUPTED);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_wait(ep, 5000000),
                              (int)MOQ_ERR_INTERRUPTED);
        /* drain() honors the latch too, ahead of any backend probe: it must
         * return INTERRUPTED immediately, not block for the timeout. */
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_drain(ep, 5000000),
                              (int)MOQ_ERR_INTERRUPTED);
        /* Non-terminal: clearing restores normal blocking. */
        moq_endpoint_set_interrupted(ep, false);
        moq_result_t rc = moq_endpoint_wait(ep, 50000);
        MOQ_TEST_CHECK(rc == MOQ_OK || rc == MOQ_DONE);
        MOQ_TEST_CHECK(!moq_endpoint_is_closed(ep));

        /* Interrupt WHILE blocked: another thread sets the latch ~100ms in;
         * a waiter blocked in a 5s wait must observe INTERRUPTED promptly --
         * well before any 5s timeout could expire. A wait may legitimately
         * complete with MOQ_OK/MOQ_DONE from unrelated pump activity BEFORE
         * the latch is set (especially under parallel-suite load), so loop
         * until INTERRUPTED and bound the total elapsed time instead of
         * asserting on a single call. */
        latch_arg_t la = { ep, 100 };
        pthread_t th;
        MOQ_TEST_CHECK_EQ_INT(pthread_create(&th, NULL, latch_setter, &la), 0);
        uint64_t t0 = (uint64_t)time(NULL);
        rc = MOQ_OK;
        while (rc != MOQ_ERR_INTERRUPTED &&
               (uint64_t)time(NULL) - t0 < 4) {
            rc = moq_endpoint_wait(ep, 5000000);
            if (rc == MOQ_ERR_CLOSED) break;
        }
        uint64_t elapsed = (uint64_t)time(NULL) - t0;
        pthread_join(th, NULL);
        MOQ_TEST_CHECK_EQ_INT((int)rc, (int)MOQ_ERR_INTERRUPTED);
        MOQ_TEST_CHECK(elapsed < 4);   /* prompt break, never a 5s timeout */
        moq_endpoint_set_interrupted(ep, false);
    }

    /* == post(): FIFO + exactly-once on the network thread ============ */
    int fifo_order_a = -1, fifo_order_b = -1;   /* reasserted after stop() */
    {
        task_ctx_t *ta = &g_task_a;      /* static: see LIFETIME */
        task_ctx_t *tb = &g_task_b;
        atomic_init(&g_task_order, 0);
        task_ctx_reset(ta, &g_task_order, 1);
        task_ctx_reset(tb, &g_task_order, 2);

        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_post(ep, task_fn, ta),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_post(ep, task_fn, tb),
                              (int)MOQ_OK);
        int waited = 0;
        while ((task_ctx_done(ta) == 0 || task_ctx_done(tb) == 0) &&
               waited < 5000) {
            (void)moq_endpoint_wait(ep, 100000);
            waited += 100;
        }
        /* acquire completion, then observe; the draws are recorded so the
         * post-terminal reassertion can prove they never moved */
        check_fifo_post("live", -1, -1);
        fifo_order_a = task_ctx_get(&ta->order_seen);
        fifo_order_b = task_ctx_get(&tb->order_seen);
    }

    /* == stop: idempotent; post-after-stop never runs fn =============== */
    {
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(ep), (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(ep), (int)MOQ_OK);
        MOQ_TEST_CHECK(moq_endpoint_is_closed(ep));
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_state(ep),
                              (int)MOQ_ENDPOINT_CLOSED);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_wait(ep, 1000),
                              (int)MOQ_ERR_CLOSED);
        /* post-after-stop: refused, caller keeps ctx, fn NEVER runs. The ctx
         * is static anyway, so even a contract violation could not corrupt
         * the stack -- it would show up as done != 0. */
        task_ctx_t *t = &g_task_after_stop;
        task_ctx_reset(t, NULL, 3);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_post(ep, task_fn, t),
                              (int)MOQ_ERR_CLOSED);
        MOQ_TEST_CHECK_EQ_INT(task_ctx_done(t), 0);

        /* == exactly-once THROUGH the terminal drain ==================== *
         * stop() has drained every accepted task, so no further invocation
         * is possible. Reassert the complete postconditions here: a
         * duplicate that arrived after the first observation -- during a
         * later pump cycle or in the drain itself -- is invisible to a check
         * taken at first completion, and visible here. */
        check_fifo_post("post-stop", fifo_order_a, fifo_order_b);

        {
            version_probe_t *vp = &g_version_probe;
            int vr = atomic_load_explicit(&vp->ran, memory_order_acquire);
            MOQ_TEST_CHECK_EQ_INT(vr, 1);            /* still exactly once */
            MOQ_TEST_CHECK_EQ_INT(
                atomic_load_explicit(&vp->saw_null_session,
                                     memory_order_relaxed), 0);
            MOQ_TEST_CHECK_EQ_INT(
                atomic_load_explicit(&vp->session_version,
                                     memory_order_relaxed),
                (int)MOQ_VERSION_DRAFT_16);
            MOQ_TEST_CHECK_EQ_INT(
                atomic_load_explicit(&vp->ep_version, memory_order_relaxed),
                atomic_load_explicit(&vp->session_version,
                                     memory_order_relaxed));
        }

        moq_endpoint_destroy(ep);
        ep = NULL;

        /* destroy cannot resurrect a task either */
        check_fifo_post("post-destroy", fifo_order_a, fifo_order_b);
        MOQ_TEST_CHECK_EQ_INT(
            atomic_load_explicit(&g_version_probe.ran, memory_order_acquire),
            1);
    }

    /* == Deterministic NULL-session terminal drain ==================== *
     * A WEBTRANSPORT endpoint's on_pump never runs before WT CONNECT is
     * accepted; aim it at a bound-but-silent UDP socket so no pump cycle
     * ever fires, post a task, then stop(): the accepted task MUST drain
     * exactly once with session == NULL. (Skipped if the WT facade is not
     * in this build -- connect returns UNSUPPORTED then.) */
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        MOQ_TEST_CHECK(sock >= 0);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        MOQ_TEST_CHECK_EQ_INT(bind(sock, (struct sockaddr *)&sa, sizeof(sa)), 0);
        socklen_t slen = sizeof(sa);
        MOQ_TEST_CHECK_EQ_INT(getsockname(sock, (struct sockaddr *)&sa, &slen), 0);
        int silent_port = (int)ntohs(sa.sin_port);

        char wurl[64];
        moq_endpoint_cfg_t wc;
        moq_endpoint_cfg_init(&wc);
        snprintf(wurl, sizeof(wurl), "https://127.0.0.1:%d/moq", silent_port);
        wc.url = (moq_bytes_t){ (const uint8_t *)wurl, strlen(wurl) };
        wc.insecure_skip_verify = true;
        static const moq_version_t v16b = MOQ_VERSION_DRAFT_16;
        wc.versions.struct_size = sizeof(moq_version_offer_t);
        wc.versions.policy = MOQ_VERSION_POLICY_EXACT;
        wc.versions.versions = &v16b;
        wc.versions.version_count = 1;

        moq_endpoint_t *wep = NULL;
        moq_result_t crc = moq_endpoint_connect(&wc, &wep);
        if (crc == MOQ_OK) {
            task_ctx_t *t = &g_task_wt;   /* static: drains at stop() */
            task_ctx_reset(t, NULL, 4);
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_post(wep, task_fn, t),
                                  (int)MOQ_OK);
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(wep), (int)MOQ_OK);
            /* after stop(): the accepted task drained exactly once, with the
             * NULL-session marker and no live marker */
            int dw = task_ctx_done(t);
            MOQ_TEST_CHECK_EQ_INT(dw, 1);                 /* exactly once */
            if (dw >= 1) {
                MOQ_TEST_CHECK_EQ_INT(
                    task_ctx_get(&t->saw_null_session), 1);   /* NULL marker */
                MOQ_TEST_CHECK_EQ_INT(
                    task_ctx_get(&t->saw_live_session), 0);
            }

            moq_endpoint_destroy(wep);

            /* ... and destroy cannot resurrect it. The context is static and
             * every field atomic, so observing it after the endpoint is gone
             * is valid; the COMPLETE postcondition is reasserted, not just
             * the count, so a duplicate arriving during destroy that also
             * flipped a marker is caught. */
            int dwd = task_ctx_done(t);
            MOQ_TEST_CHECK_EQ_INT(dwd, 1);
            MOQ_TEST_CHECK_EQ_INT(task_ctx_get(&t->saw_null_session), 1);
            MOQ_TEST_CHECK_EQ_INT(task_ctx_get(&t->saw_live_session), 0);
        } else {
            MOQ_TEST_CHECK_EQ_INT((int)crc, (int)MOQ_ERR_UNSUPPORTED);
        }
        close(sock);
    }

    /* == Pre-session wake: the public nudge wakes a CONNECTING waiter == *
     * A raw-QUIC client offer (AUTO = every supported version) defers the
     * session until ALPN negotiation; against a silent UDP peer it stays
     * session-less. The wake contract is not session-gated: the nudged
     * pump cycle must mark activity, so a parked moq_endpoint_wait()
     * returns MOQ_OK promptly -- not CLOSED at the ~30s handshake
     * deadline, not DONE at the wait timeout. */
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        MOQ_TEST_CHECK(sock >= 0);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        MOQ_TEST_CHECK_EQ_INT(bind(sock, (struct sockaddr *)&sa, sizeof(sa)), 0);
        socklen_t slen = sizeof(sa);
        MOQ_TEST_CHECK_EQ_INT(getsockname(sock, (struct sockaddr *)&sa, &slen), 0);

        char wurl[64];
        moq_endpoint_cfg_t wc;
        moq_endpoint_cfg_init(&wc);
        snprintf(wurl, sizeof(wurl), "moqt://127.0.0.1:%d",
                 (int)ntohs(sa.sin_port));
        wc.url = (moq_bytes_t){ (const uint8_t *)wurl, strlen(wurl) };
        wc.insecure_skip_verify = true;

        moq_endpoint_t *wep = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&wc, &wep), (int)MOQ_OK);
        /* Drain activity retained from loop startup. */
        while (moq_endpoint_wait(wep, 1000) == MOQ_OK) {}
        moq_endpoint_wake(wep);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_wait(wep, 45ULL * 1000 * 1000),
                              (int)MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(wep), (int)MOQ_OK);
        moq_endpoint_destroy(wep);
        close(sock);
    }

    /* == Version negotiation matrix ==================================== *
     * The v0 threaded server accepts exactly one connection, so every
     * scenario below runs against its own fresh d16-only (or d18-capable)
     * server. */

    moq_pq_threaded_stop(srv);
    moq_pq_threaded_destroy(srv);
    srv = NULL;

    /* LIST {18, 16} against a d16-only server: the server's selection of
     * moqt-16 wins and the session establishes at draft-16 -- never a
     * silent downgrade, an honest negotiation. */
    {
        int nport = 0;
        moq_pq_threaded_t *nsrv = start_server(cert, key, &nport);
        MOQ_TEST_CHECK(nsrv != NULL);
        char nurl[64];
        moq_endpoint_cfg_t nc = client_cfg(nurl, sizeof(nurl), nport);
        static const moq_version_t both[] = {
            MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_16 };
        nc.versions.struct_size = sizeof(moq_version_offer_t);
        nc.versions.policy = MOQ_VERSION_POLICY_LIST;
        nc.versions.versions = both;
        nc.versions.version_count = 2;
        moq_endpoint_t *nep = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&nc, &nep), (int)MOQ_OK);
        MOQ_TEST_CHECK(wait_for_state(nep, MOQ_ENDPOINT_ESTABLISHED, 15000));
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(nep),
                              (int)MOQ_VERSION_DRAFT_16);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(nep), (int)MOQ_OK);
        moq_endpoint_destroy(nep);
        moq_pq_threaded_stop(nsrv);
        moq_pq_threaded_destroy(nsrv);
    }

    /* EXACT 18 against a d16-only server: no ALPN overlap, the handshake
     * fails, and the endpoint terminates fatally -- never a silent 16. */
    {
        int nport = 0;
        moq_pq_threaded_t *nsrv = start_server(cert, key, &nport);
        MOQ_TEST_CHECK(nsrv != NULL);
        char nurl[64];
        moq_endpoint_cfg_t nc = client_cfg(nurl, sizeof(nurl), nport);
        static const moq_version_t v18 = MOQ_VERSION_DRAFT_18;
        nc.versions.struct_size = sizeof(moq_version_offer_t);
        nc.versions.policy = MOQ_VERSION_POLICY_EXACT;
        nc.versions.versions = &v18;
        nc.versions.version_count = 1;
        moq_endpoint_t *nep = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&nc, &nep), (int)MOQ_OK);
        MOQ_TEST_CHECK(wait_for_state(nep, MOQ_ENDPOINT_CLOSED, 20000));
        MOQ_TEST_CHECK(moq_endpoint_is_fatal(nep));
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(nep), 0);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(nep), (int)MOQ_OK);
        moq_endpoint_destroy(nep);
        moq_pq_threaded_stop(nsrv);
        moq_pq_threaded_destroy(nsrv);
    }

    /* RAW explicit SNI override (different from the host) is honored now:
     * carried as the TLS server name while connecting to the IP host. */
    {
        int nport = 0;
        moq_pq_threaded_t *nsrv = start_server(cert, key, &nport);
        MOQ_TEST_CHECK(nsrv != NULL);
        char nurl[64];
        moq_endpoint_cfg_t nc = client_cfg(nurl, sizeof(nurl), nport);
        nc.sni = (moq_bytes_t){ (const uint8_t *)"localhost", 9 };
        moq_endpoint_t *nep = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&nc, &nep), (int)MOQ_OK);
        MOQ_TEST_CHECK(wait_for_state(nep, MOQ_ENDPOINT_ESTABLISHED, 15000));
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(nep), (int)MOQ_OK);
        moq_endpoint_destroy(nep);
        moq_pq_threaded_stop(nsrv);
        moq_pq_threaded_destroy(nsrv);
    }

    /* == AUTO against a d18-CAPABLE server ESTABLISHES at 18 =========== *
     * The server offers {18, 16}; the endpoint's AUTO offer leads with 18
     * and the client's order decides: both sides negotiate moqt-18 and the
     * draft-18 session establishes over the real transport (the symmetric
     * uni-control handshake end to end through the managed facades). */
    {
        static const char *both_alpn[] = { "moqt-18", "moqt-16" };
        int port18 = 0;
        moq_pq_threaded_t *srv18 =
            start_server_ex(cert, key, both_alpn, 2, &port18);
        MOQ_TEST_CHECK(srv18 != NULL);
        if (srv18) {
            char nurl[64];
            moq_endpoint_cfg_t nc = client_cfg(nurl, sizeof(nurl), port18);
            moq_endpoint_t *nep = NULL;
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&nc, &nep),
                                  (int)MOQ_OK);
            MOQ_TEST_CHECK(wait_for_state(nep, MOQ_ENDPOINT_ESTABLISHED,
                                          15000));
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_negotiated_version(nep),
                                  (int)MOQ_VERSION_DRAFT_18);
            MOQ_TEST_CHECK_EQ_INT(
                (int)moq_pq_threaded_negotiated_version(srv18),
                (int)MOQ_VERSION_DRAFT_18);
            MOQ_TEST_CHECK(!moq_endpoint_is_fatal(nep));
            MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(nep), (int)MOQ_OK);
            moq_endpoint_destroy(nep);
            moq_pq_threaded_stop(srv18);
            moq_pq_threaded_destroy(srv18);
        }
    }

    /* == Facade-level regression test: negotiation carries draft-18 ===== *
     * A direct moq_pq_threaded client (no endpoint) offering {18, 16}
     * against the d18-capable server: ALPN selects moqt-18 and BOTH
     * facades map it to draft-18 sessions. Establishment itself is
     * asserted at the endpoint level above; this pins the bare facade's
     * select -> version path (the session is network-thread-confined, so
     * no state peek here). */
    {
        static const char *both_alpn[] = { "moqt-18", "moqt-16" };
        int port18 = 0;
        moq_pq_threaded_t *srv18 =
            start_server_ex(cert, key, both_alpn, 2, &port18);
        MOQ_TEST_CHECK(srv18 != NULL);
        if (srv18) {
            moq_pq_threaded_cfg_t cc;
            moq_pq_threaded_cfg_init(&cc);
            cc.alloc = moq_alloc_default();
            cc.perspective = MOQ_PERSPECTIVE_CLIENT;
            cc.host = "127.0.0.1";
            cc.port = port18;
            cc.insecure_skip_verify = true;
            cc.send_request_capacity = true;
            cc.initial_request_capacity = 16;
            cc.on_lane_pump = server_pump;
            cc.alpn_list = both_alpn;
            cc.alpn_count = 2;
            moq_pq_threaded_t *cli = NULL;
            MOQ_TEST_CHECK_EQ_INT((int)moq_pq_threaded_create(&cc, &cli),
                                  (int)MOQ_OK);
            if (cli) {
                int waited = 0;
                while ((moq_pq_threaded_negotiated_version(cli) == 0 ||
                        moq_pq_threaded_negotiated_version(srv18) == 0) &&
                       !moq_pq_threaded_is_fatal(cli) && waited < 10000) {
                    (void)moq_pq_threaded_wait(cli, 100000);
                    waited += 100;
                }
                MOQ_TEST_CHECK_EQ_INT(
                    (int)moq_pq_threaded_negotiated_version(cli),
                    (int)MOQ_VERSION_DRAFT_18);
                MOQ_TEST_CHECK_EQ_INT(
                    (int)moq_pq_threaded_negotiated_version(srv18),
                    (int)MOQ_VERSION_DRAFT_18);
                MOQ_TEST_CHECK(!moq_pq_threaded_is_fatal(cli));
                MOQ_TEST_CHECK(!moq_pq_threaded_is_fatal(srv18));
                moq_pq_threaded_stop(cli);
                moq_pq_threaded_destroy(cli);
            }
            moq_pq_threaded_stop(srv18);
            moq_pq_threaded_destroy(srv18);
        }
    }

    /* == network-thread startup death is an error, never a zombie ====== *
     * The packet loop opens its sockets on its own thread; with the port
     * already taken the loop dies before ready. create() must observe
     * that and fail -- the old behavior returned a facade that would
     * never connect and never report. */
    {
        int port = 0;
        moq_pq_threaded_t *srv = start_server(cert, key, &port);
        MOQ_TEST_CHECK(srv != NULL);
        if (!srv) return 1;

        moq_pq_threaded_cfg_t cfg;
        moq_pq_threaded_cfg_init(&cfg);
        cfg.alloc = moq_alloc_default();
        cfg.perspective = MOQ_PERSPECTIVE_SERVER;
        cfg.cert_path = cert;
        cfg.key_path = key;
        cfg.port = port;            /* same port: bind must fail */
        cfg.on_lane_pump = server_pump;
        moq_pq_threaded_t *dup = NULL;
        MOQ_TEST_CHECK(moq_pq_threaded_create(&cfg, &dup) != MOQ_OK);
        MOQ_TEST_CHECK(dup == NULL);

        moq_pq_threaded_stop(srv);
        moq_pq_threaded_destroy(srv);
    }

    /* == Endpoint sharing: one receiver + one sender coexist =========== *
     * The v0 contract (per-kind hook slots): at most one receiver AND at
     * most one sender per endpoint, and BOTH may be attached at once. The
     * lifecycle is independent per attachment: stop() is gated while
     * EITHER is live, destroying one never detaches the other, and
     * wake/drain keep working with both attached. (The test server's pump
     * never answers announces/subscribes, so neither service progresses
     * past setup -- these are lifecycle assertions only.) */
    {
        int sport = 0;
        moq_pq_threaded_t *ssrv = start_server(cert, key, &sport);
        MOQ_TEST_CHECK(ssrv != NULL);
        if (!ssrv) return 1;

        char surl[64];
        moq_endpoint_cfg_t sc = client_cfg(surl, sizeof(surl), sport);
        moq_endpoint_t *sep = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&sc, &sep), (int)MOQ_OK);
        MOQ_TEST_CHECK(wait_for_state(sep, MOQ_ENDPOINT_ESTABLISHED, 15000));

        static const moq_bytes_t ns_parts[] = {
            { (const uint8_t *)"share", 5 }, { (const uint8_t *)"test", 4 }
        };
        moq_namespace_t ns = { ns_parts, 2 };

        moq_media_receiver_cfg_t rcfg;
        moq_media_receiver_cfg_init_live(&rcfg);
        rcfg.namespace_ = ns;
        moq_media_receiver_t *rx = NULL;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_media_receiver_attach(sep, &rcfg, &rx), (int)MOQ_OK);
        MOQ_TEST_CHECK(rx != NULL);

        moq_media_sender_cfg_t scfg2;
        moq_media_sender_cfg_init_live(&scfg2);
        scfg2.namespace_ = ns;
        moq_media_sender_t *tx = NULL;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_media_sender_attach(sep, &scfg2, &tx), (int)MOQ_OK);
        MOQ_TEST_CHECK(tx != NULL);

        /* Per-kind slots stay single: a SECOND receiver or sender is
         * refused while the first is attached. */
        {
            moq_media_receiver_t *rx2 = NULL;
            MOQ_TEST_CHECK_EQ_INT(
                (int)moq_media_receiver_attach(sep, &rcfg, &rx2),
                (int)MOQ_ERR_WRONG_STATE);
            MOQ_TEST_CHECK(rx2 == NULL);
            moq_media_sender_t *tx2 = NULL;
            MOQ_TEST_CHECK_EQ_INT(
                (int)moq_media_sender_attach(sep, &scfg2, &tx2),
                (int)MOQ_ERR_WRONG_STATE);
            MOQ_TEST_CHECK(tx2 == NULL);
        }

        /* stop() is gated while both live. */
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(sep),
                              (int)MOQ_ERR_WRONG_STATE);

        /* wake with both attached: safe, and the endpoint stays healthy. */
        moq_endpoint_wake(sep);
        MOQ_TEST_CHECK(!moq_endpoint_is_fatal(sep));

        /* Destroying the receiver does NOT detach or kill the sender:
         * stop() stays gated and the sender handle stays queryable. */
        moq_media_receiver_destroy(rx);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(sep),
                              (int)MOQ_ERR_WRONG_STATE);
        MOQ_TEST_CHECK(!moq_media_sender_is_fatal(tx));
        (void)moq_media_sender_is_ready(tx);   /* queryable, value untested */

        /* The freed receiver slot is reusable while the sender lives. */
        {
            moq_media_receiver_t *rx3 = NULL;
            MOQ_TEST_CHECK_EQ_INT(
                (int)moq_media_receiver_attach(sep, &rcfg, &rx3), (int)MOQ_OK);
            moq_media_receiver_destroy(rx3);
        }

        /* drain with the sender still attached returns a documented code
         * (nothing was written, so OK is expected on picoquic; DONE also
         * legal under load). */
        {
            moq_result_t drc = moq_endpoint_drain(sep, 2000000);
            MOQ_TEST_CHECK(drc == MOQ_OK || drc == MOQ_DONE);
        }

        moq_media_sender_destroy(tx);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(sep), (int)MOQ_OK);
        moq_endpoint_destroy(sep);
        moq_pq_threaded_stop(ssrv);
        moq_pq_threaded_destroy(ssrv);
    }

    /* == moq_media_sender_wait: level-triggered wait contract ========== *
     * Deterministic cases only:
     *   - NULL sender          -> MOQ_ERR_INVAL
     *   - not ready            -> MOQ_DONE on a zero/short timeout (the
     *                             test server never answers the announce)
     *   - interrupt latch set  -> MOQ_ERR_INTERRUPTED
     * The ready+space MOQ_OK fast path is covered by the white-box test in
     * test_media_sender_catalog.c (readiness needs a relay accept). */
    {
        MOQ_TEST_CHECK_EQ_INT((int)moq_media_sender_wait(NULL, 0),
                              (int)MOQ_ERR_INVAL);

        int wport = 0;
        moq_pq_threaded_t *wsrv = start_server(cert, key, &wport);
        MOQ_TEST_CHECK(wsrv != NULL);
        if (!wsrv) return 1;
        char wurl[64];
        moq_endpoint_cfg_t wc = client_cfg(wurl, sizeof(wurl), wport);
        moq_endpoint_t *wep = NULL;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_connect(&wc, &wep), (int)MOQ_OK);
        MOQ_TEST_CHECK(wait_for_state(wep, MOQ_ENDPOINT_ESTABLISHED, 15000));

        static const moq_bytes_t wns_parts[] = {
            { (const uint8_t *)"wait", 4 }, { (const uint8_t *)"test", 4 }
        };
        moq_namespace_t wns = { wns_parts, 2 };
        moq_media_sender_cfg_t wcfg;
        moq_media_sender_cfg_init_live(&wcfg);
        wcfg.namespace_ = wns;
        moq_media_sender_t *ws = NULL;
        MOQ_TEST_CHECK_EQ_INT(
            (int)moq_media_sender_attach(wep, &wcfg, &ws), (int)MOQ_OK);

        /* Not ready (announce never answered): the level never holds, so
         * once residual endpoint activity drains, the wait reports MOQ_DONE
         * (MOQ_OK on a wake means "re-check", same as receiver_wait; it must
         * settle to DONE on a quiescent endpoint, never spuriously report
         * the level). */
        MOQ_TEST_CHECK(!moq_media_sender_is_ready(ws));
        {
            bool saw_done = false;
            for (int i = 0; i < 50 && !saw_done; i++)
                saw_done = (moq_media_sender_wait(ws, 50000) == MOQ_DONE);
            MOQ_TEST_CHECK(saw_done);
        }

        /* Latched: immediate MOQ_ERR_INTERRUPTED, repeatably (sticky). */
        moq_endpoint_set_interrupted(wep, true);
        MOQ_TEST_CHECK_EQ_INT((int)moq_media_sender_wait(ws, 5000000),
                              (int)MOQ_ERR_INTERRUPTED);
        MOQ_TEST_CHECK_EQ_INT((int)moq_media_sender_wait(ws, 5000000),
                              (int)MOQ_ERR_INTERRUPTED);
        moq_endpoint_set_interrupted(wep, false);
        {
            bool saw_done = false;
            for (int i = 0; i < 50 && !saw_done; i++)
                saw_done = (moq_media_sender_wait(ws, 50000) == MOQ_DONE);
            MOQ_TEST_CHECK(saw_done);
        }

        moq_media_sender_destroy(ws);
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_stop(wep), (int)MOQ_OK);
        moq_endpoint_destroy(wep);
        moq_pq_threaded_stop(wsrv);
        moq_pq_threaded_destroy(wsrv);
    }

    MOQ_TEST_PASS("endpoint_lifecycle");
    return failures != 0;
}
