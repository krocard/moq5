/*
 * MOQ5 Relay: a deterministic MoQ relay over the MsQuic managed server.
 * Sessions live on their lane's thread; the production binding
 * (bind/moqr_bind.h) runs inside on_lane_pump and is the only code that
 * touches them. Default is one lane / one shard (the single-core production
 * path); listener.lanes > 1 partitions connections across N independent
 * lock-domain lanes, each driving one relay shard.
 *
 * Usage:
 *   moq5-relay serve    --config relay.json
 *   moq5-relay capacity --config relay.json     (print the ceiling, exit)
 */

#include "cliargs.h"
#include "config.h"
#include "conn_reap.h"
#include "versions.h"
#include "lanestats.h"
#include "snapshot.h"
#ifdef MOQR_BIND_TESTING
#include "blockedstats.h"   /* verify build only: RELAY_BLOCKED_V0 emission */
#endif
#ifdef MOQR_VERIFY_SEAM
/* The constrained-pool seam is compiled into two binaries (config.h): the
 * reason-proof verify build and the symmetric timing build. Their messages
 * self-identify so a harness log never leaves which binary ran ambiguous. */
#ifdef MOQR_BIND_TESTING
#define MOQR_SEAM_NAME "moq-relay-verify"
#else
#define MOQR_SEAM_NAME "moq-relay-measure"
#endif
#endif

#include "../bind/moqr_bind.h"
#include "../shard/moqr_shards.h"

#include <moqr_obs.h>

#include <moq/msquic_managed.h>

#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The signal flags are written from a signal handler (delivered on an
 * arbitrary thread) and read from both the lane thread (relay_lane_pump)
 * and the main thread (the wait loop). volatile sig_atomic_t is only
 * async-signal-safe within one thread, not a cross-thread synchronization
 * primitive — so use lock-free atomics. int is lock-free on every supported
 * target, which a handler may safely touch (C11 7.14.1.1). */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "signal flags require an always-lock-free atomic_int");
/* ATOMIC_INT_LOCK_FREE governs atomic_uint too (C11 groups signed/unsigned
 * int), so the multi-lane dump epochs below are equally handler-safe. */

/* The config lane cap is decoupled from the shard runtime; prove they agree
 * here, the one TU that sees both headers. */
_Static_assert(MOQR_CLI_MAX_LANES == MOQR_SHARDS_MAX,
               "listener.lanes cap must equal the shard runtime cap");

static atomic_int g_stop;
static atomic_int g_dump_state;   /* SIGUSR1: metrics + routes (single-lane) */
static atomic_int g_dump_trace;   /* SIGUSR2: trace JSONL      (single-lane) */

/* Multi-lane dumps: a handler can't walk a per-lane heap ctx, so it only bumps
 * a lock-free epoch; each lane's pump renders its own shard once when it
 * observes a new epoch (compared with !=, so wrap is harmless). */
static atomic_uint g_dump_state_epoch;
static atomic_uint g_dump_trace_epoch;

static void
on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_stop, 1);
}

/* Async-signal-safe: only latch a flag; the dump is rendered on the lane
 * thread inside on_lane_pump, where the core/binding/trace are safe to read. */
static void
on_dump_signal(int sig)
{
    if (sig == SIGUSR1) {
        atomic_store(&g_dump_state, 1);
    } else if (sig == SIGUSR2) {
        atomic_store(&g_dump_trace, 1);
    }
}

/* Multi-lane variant: async-signal-safe fetch_add only. */
static void
on_dump_signal_lanes(int sig)
{
    if (sig == SIGUSR1) {
        atomic_fetch_add(&g_dump_state_epoch, 1u);
    } else if (sig == SIGUSR2) {
        atomic_fetch_add(&g_dump_trace_epoch, 1u);
    }
}

/* Readiness implies a graceful stop.
 *
 * The handlers go in BEFORE the listening record is published, because that
 * record is what an operator waits on: an immediate SIGTERM must take the
 * graceful path that drains, emits terminal lane statistics and exits 0, not
 * the default disposition. Publishing readiness first leaves a window in which
 * the advertised process dies abruptly instead.
 *
 * `g_stop` is cleared here as well: the flag is process-global, and a serve
 * invocation must never inherit a stop latched before it started.
 */
static void
serve_install_signals(void (*dump)(int))
{
    atomic_store(&g_stop, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, dump);
    signal(SIGUSR2, dump);
}

typedef struct serve_ctx {
    moqr_bind_t      *bind;
    moqr_core_t      *core;
    moqr_trace_t     *trace;
    moqr_obs_labels_t labels;
} serve_ctx_t;

/* Render `fn` into a heap buffer, growing once on truncation (the serializers
 * report the size they need), then write it to stderr under a banner. Network
 * thread only. The buffer is transient — allocated and freed per dump — so it
 * is not part of the process capacity ceiling. */
typedef moqr_result_t (*dump_fn)(void *ctx, char *buf, size_t cap,
                                 size_t *written);

static void
dump_emit(const char *label, dump_fn fn, void *ctx)
{
    size_t cap = 16u * 1024u;
    char  *buf = malloc(cap);
    if (buf == NULL) {
        return;
    }
    size_t w = 0;
    moqr_result_t rc = fn(ctx, buf, cap, &w);
    if (rc == MOQR_ERR_CAPACITY) {
        char *bigger = realloc(buf, w + 1);
        if (bigger != NULL) {
            buf = bigger;
            rc = fn(ctx, buf, w + 1, &w);
        }
    }
    if (rc == MOQR_OK) {
        fprintf(stderr, "\n===== MOQ5 Relay %s =====\n%s\n", label, buf);
    }
    free(buf);
}

static moqr_result_t
dump_routes_fn(void *ctx, char *b, size_t c, size_t *w)
{
    return moqr_core_route_dump_text((moqr_core_t *)ctx, b, c, w);
}

static moqr_result_t
dump_trace_fn(void *ctx, char *b, size_t c, size_t *w)
{
    return moqr_trace_write_jsonl((moqr_trace_t *)ctx, b, c, w);
}

static moqr_result_t
dump_metrics_fn(void *vctx, char *b, size_t c, size_t *w)
{
    serve_ctx_t *ctx = vctx;
    moqr_core_stats_t cs;
    moqr_core_get_stats(ctx->core, &cs);
    moqr_bind_stats_t bs;
    moqr_bind_get_stats(ctx->bind, &bs);
    return moqr_metrics_write_prometheus(&cs, &bs, &ctx->labels, b, c, w);
}

/* Owning-lane journal dump (multi-lane only; K=1 has no journal). */
typedef struct dump_journal_ctx {
    moqr_shards_t *shards;
    uint16_t       shard;
} dump_journal_ctx_t;

static moqr_result_t
dump_journal_fn(void *vctx, char *b, size_t c, size_t *w)
{
    dump_journal_ctx_t *j = vctx;
    return moqr_shards_journal_dump_text(j->shards, j->shard, b, c, w);
}

/* Per-connection relay state is a TAG in the adapter's conn_user slot (never a
 * map keyed by session/conn pointer — those values can be reused by a successor
 * connection). Nothing is allocated, so there is nothing to free at reap.
 *   NULL   -> never seen: attach to the binding on first sight.
 *   OPENED -> attached; the binding owns per-connection state from here.
 *   DEAD   -> the binding detached it (SESSION_CLOSED) or refused it: never
 *             re-attach, even while the terminal connection stays visible for
 *             its final pump batch. */
#define RELAY_CONN_OPENED MOQR_CONN_OPENED
#define RELAY_CONN_DEAD   MOQR_CONN_DEAD

/* Lane thread, per pump: attach new connections, run the binding, then
 * service any pending operator dump signals. Single lane (lane 0) drives the
 * one relay binding; every accepted session lives on this lane. */
static int
relay_lane_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
                uint64_t now_us, void *vctx)
{
    serve_ctx_t *ctx = vctx;
    (void)m;
    if (moq_msquic_lane_index(lane) != 0) {
        return 0;   /* single-lane relay: only lane 0 carries connections */
    }
    for (moq_msquic_managed_conn_t *conn = moq_msquic_lane_next_conn(lane,
                                                                     NULL);
         conn != NULL; conn = moq_msquic_lane_next_conn(lane, conn)) {
        void *tag = moq_msquic_managed_conn_user(conn);
        if (tag == RELAY_CONN_DEAD) {
            continue;
        }
        moq_session_t *s = moq_msquic_managed_conn_session(conn);
        if (s == NULL) {
            continue;
        }
        if (tag == NULL) {
            if (moqr_bind_conn_open(ctx->bind, s,
                                    moq_msquic_managed_conn_negotiated_version(conn)) == MOQR_OK) {
                moq_msquic_managed_conn_set_user(conn, RELAY_CONN_OPENED);
            } else {
                /* Bind table full: refuse the connection outright. */
                moq_msquic_managed_conn_set_user(conn, RELAY_CONN_DEAD);
                moq_msquic_managed_conn_close(conn, 0);
            }
        }
    }
    (void)moqr_bind_pump(ctx->bind, now_us);
    /* A binding this pump's own work detached must be retired now: nothing
     * else wakes this lane on its behalf. */
    if (!moqr_relay_reap_pass(ctx->bind, lane, NULL)) {
        fprintf(stderr, "MOQ5 Relay: connection retirement failed — stopping\n");
        return 1;
    }

    if (atomic_exchange(&g_dump_state, 0)) {
        dump_emit("metrics", dump_metrics_fn, ctx);
        dump_emit("routes", dump_routes_fn, ctx->core);
    }
    if (atomic_exchange(&g_dump_trace, 0)) {
        dump_emit("trace", dump_trace_fn, ctx->trace);
    }
    return atomic_load(&g_stop) ? 1 : 0;
}

/* Prints the relay-state allocation-request ceiling for THIS config, in
 * the composition production actually allocates (direct core+bind+trace at
 * lanes=1; the full shard runtime + CLI context/rows at lanes>1). Returns
 * nonzero when the config cannot be described (create would refuse too). */
/* Emit the strict versioned operating-point record from the RESOLVED
 * limits (the resolver's output, never the request): the summarizer joins
 * these four values into the workload fingerprint, so runs at different
 * operating points can never pool. Emitted once, at serve start, on every
 * serve path. A resolve failure is unreachable here (the preflight already
 * validated this config), but fail closed with a refusal line anyway. */
static void
serve_print_run_config(const moqr_shards_cfg_t *scfg)
{
    moqr_shards_limits_t lim;
    if (moqr_shards_cfg_resolve(scfg, &lim) != MOQR_OK) {
        printf(MOQR_RUN_CONFIG_PREFIX ",refused=resolver\n");
        return;
    }
    moqr_cli_run_config_row_t rc;
    memset(&rc, 0, sizeof(rc));
    rc.pump_turn_messages = lim.pump_turn_msgs;
    rc.pump_turn_bytes = lim.pump_turn_bytes;
    rc.demand_channel_entries = lim.dch_cap;
    rc.demand_channel_bytes = lim.dch_byte_cap;
    char line[512];
    int len = moqr_cli_run_config_format(line, sizeof(line), &rc);
    if (len > 0 && (size_t)len < sizeof(line)) {
        printf("%s\n", line);
    } else {
        printf(MOQR_RUN_CONFIG_PREFIX ",refused=format\n");
    }
}

static int
print_capacity(const moqr_cli_config_t *cfg, size_t serve_ctx_bytes)
{
    moqr_cli_capacity_t cap;
    if (moqr_cli_describe_capacity(cfg, moq_alloc_default(), serve_ctx_bytes,
                                   &cap) != MOQR_OK) {
        fprintf(stderr,
                "capacity: refused — the ceiling overflows a 64-bit byte "
                "count (or the budgets cannot fit the lanes); refusing "
                "rather than printing an understated number\n");
        return 1;
    }
    printf("relay-state allocation-request ceiling: %" PRIu64 " bytes\n",
           cap.total_bytes);
    printf("  (allocator requests only — excludes malloc metadata, "
           "size-class rounding,\n   fragmentation, thread stacks, "
           "transient diagnostics, and MsQuic/session\n   memory that "
           "remains adapter-owned)\n");
    printf("  per shard: structure %" PRIu64 " + payloads %" PRIu64
           " + binding %" PRIu64 " + trace %" PRIu64 " bytes\n",
           cap.core_structure_bytes, cap.core_payload_bytes,
           cap.bind_structure_bytes, cap.trace_bytes);
    if (cap.cross_shard_bytes != 0) {
        printf("  cross-shard: %" PRIu64
               " bytes (runtime, channels, canon keys, staging)\n",
               cap.cross_shard_bytes);
    }
    if (cap.cli_runtime_bytes != 0) {
        printf("  cli runtime: %" PRIu64 " bytes (serve context + snapshot "
               "rows; included above)\n",
               cap.cli_runtime_bytes);
    }
    printf("  usable client bindings per shard: %u\n",
           cap.usable_bindings_per_shard);
    return 0;
}

static int
cmd_serve(const moqr_cli_config_t *cfg)
{
    if (cfg->cert[0] == '\0' || cfg->key[0] == '\0') {
        fprintf(stderr, "serve requires listener.cert and listener.key\n");
        return 2;
    }

    /* Assemble the core with its flight-recorder trace attached (depth from
     * telemetry.trace_ring_records). Owns trace; destroy core THEN trace. */
    moqr_trace_t *trace = NULL;
    moqr_core_t *core = NULL;
    if (moqr_cli_build_core(cfg, moq_alloc_default(), &trace, &core) !=
        MOQR_OK) {
        fprintf(stderr, "relay core create failed\n");
        return 1;
    }
    moqr_bind_cfg_t bcfg;
    moqr_bind_cfg_init_sized(&bcfg, sizeof(bcfg), moq_alloc_default());
    bcfg.core = core;
    moqr_bind_t *bind = NULL;
    if (moqr_bind_create(&bcfg, &bind) != MOQR_OK) {
        fprintf(stderr, "relay binding create failed\n");
        moqr_core_destroy(core);
        moqr_trace_destroy(trace);
        return 1;
    }
    serve_ctx_t ctx = {
        .bind = bind,
        .core = core,
        .trace = trace,
        .labels = { .shard = 0, .transport = "msquic",
                    .version = cfg->alpn_set },
    };

    moq_msquic_managed_cfg_t tcfg;
    moq_msquic_managed_cfg_init_sized(&tcfg, sizeof(tcfg));
    tcfg.alloc = moq_alloc_default();
    tcfg.perspective = MOQ_PERSPECTIVE_SERVER;
    tcfg.host = cfg->host;
    tcfg.port = (uint16_t)cfg->port;
    tcfg.cert_path = cfg->cert;
    tcfg.key_path = cfg->key;
    tcfg.insecure_skip_verify = cfg->insecure_skip_verify;
    tcfg.send_request_capacity = true;
    tcfg.initial_request_capacity = 1024;
    tcfg.streaming_objects = true;   /* stream-through forwarding: chunk-by-chunk
                                      * with live-edge delivery */
    moqr_cli_apply_versions(cfg, &tcfg);
    /* One lane == one lock domain == the one relay binding. Size the transport
     * admission cap to the binding's own resolved connection table: a defaulted
     * bind resolves max_conns from the core's max_bindings, so read the same
     * limit here rather than hard-coding a default a configured budget can move. */
    moqr_core_limits_t lim;
    moqr_core_get_limits(core, &lim);
    tcfg.lane_count = 1;
    tcfg.max_connections = lim.max_bindings;
    tcfg.on_lane_pump = relay_lane_pump;
    tcfg.on_lane_pump_user = &ctx;

    moq_msquic_managed_t *t = NULL;
    if (moq_msquic_managed_create(&tcfg, &t) != MOQ_OK) {
        fprintf(stderr, "transport create failed (port %d)\n", cfg->port);
        moqr_bind_destroy(bind);
        moqr_core_destroy(core);
        moqr_trace_destroy(trace);
        return 1;
    }

    if (print_capacity(cfg, 0) != 0) {
        /* An undescribable ceiling never serves. */
        moq_msquic_managed_destroy(t);
        moqr_bind_destroy(bind);
        moqr_core_destroy(core);
        moqr_trace_destroy(trace);
        return 2;
    }
    serve_install_signals(on_dump_signal);
    printf("MOQ5 Relay: listening on %s:%d (%s)\n", cfg->host, cfg->port,
           cfg->alpn_set);
    {
        /* lanes = 1 composes no shard runtime; report the same resolved
         * operating point the multi-lane path would have used */
        moqr_shards_cfg_t rc_cfg;
        moqr_cli_build_shards_cfg(cfg, moq_alloc_default(), &rc_cfg);
        serve_print_run_config(&rc_cfg);
    }
    printf("signals: SIGUSR1 -> metrics + route dump, "
           "SIGUSR2 -> trace JSONL (both to stderr)\n");
    fflush(stdout);
    /* A server facade outlives any single accepted connection: only our own
     * stop (SIGINT/SIGTERM -> g_stop -> the lane pump returns nonzero) is a
     * lifetime terminal. Do NOT gate on moq_msquic_managed_is_fatal(): on a
     * server that is a per-connection convenience latch (the first accepted
     * connection can set it), so gating here would let one bad client take the
     * whole relay down. wait() returns MOQ_ERR_CLOSED only on a true facade
     * terminal (stop requested or a lane-pump exit); a timeout or per-conn
     * terminal keeps the listener serving. */
    while (!atomic_load(&g_stop)) {
        if (moq_msquic_managed_wait(t, 500 * 1000) == MOQ_ERR_CLOSED) {
            break;
        }
    }
    /* stop/wait/destroy run only here, outside every adapter callback. */
    (void)moq_msquic_managed_stop(t);

    /* The single-lane attribution row: adapter doorbell counters for lane 0;
     * the shard-plane fields are TRUE zeros — the lanes=1 composition has no
     * cross-shard machinery at all (structurally inert, not refused). */
    {
        char line[2048];   /* a legal all-max V3 row is ~1.5 KiB */
        moq_msquic_lane_stats_t ad;
        memset(&ad, 0, sizeof(ad));
        ad.struct_size = (uint32_t)sizeof(ad);
        int len;
        if (moq_msquic_lane_get_stats(moq_msquic_managed_lane(t, 0), &ad,
                                      sizeof(ad)) == MOQ_OK) {
            moqr_cli_lane_stats_row_t row;
            memset(&row, 0, sizeof(row));
            row.lane = 0;
            row.wakes_same_lane = ad.wakes_same_lane;
            row.wakes_cross_lane = ad.wakes_cross_lane;
            row.wakes_external = ad.wakes_external;
            row.wakes_coalesced = ad.wakes_coalesced;
            row.pump_sweeps = ad.pump_sweeps;
            row.deadline_sweeps = ad.deadline_sweeps;
            row.idle_cap_wakes = ad.idle_cap_wakes;
            row.wake_to_pump_max_us = ad.wake_to_pump_max_us;
            row.wake_to_pump_total_us = ad.wake_to_pump_total_us;
            row.wake_to_pump_samples = ad.wake_to_pump_samples;
            row.service_passes = ad.service_passes;
            row.flush_sends = ad.flush_sends;
            row.flush_bytes = ad.flush_bytes;
            len = moqr_cli_lane_stats_format(line, sizeof(line), &row);
        } else {
            len = moqr_cli_lane_stats_format_refused(line, sizeof(line), 0,
                                                     "adapter");
        }
        if (len > 0 && (size_t)len < sizeof(line)) {
            printf("%s\n", line);
        } else {
            printf(MOQR_LANE_STATS_PREFIX ",lane=0,refused=adapter\n");
        }
    }

    moqr_bind_stats_t bs;
    moqr_bind_get_stats(bind, &bs);
    moqr_core_stats_t cs;
    moqr_core_get_stats(core, &cs);
    printf("MOQ5 Relay: stopping — conns %u, tracks %u, ingested %" PRIu64
           ", delivered %" PRIu64 ", session errors %" PRIu64 "\n",
           bs.conns, cs.tracks, cs.ingested_total, cs.delivered_total,
           bs.session_errors);

    moq_msquic_managed_destroy(t);
    moqr_bind_destroy(bind);
    moqr_core_destroy(core);
    moqr_trace_destroy(trace);   /* after the core that borrowed it */
    return 0;
}

/* -- multi-lane serve (listener.lanes > 1) --------------------------------- */

/* Lane-local state, indexed by shard == lane index. Written/read only on the
 * owning lane's pump, or by the main thread after every lane has joined. */
typedef struct serve_lanes_ctx {
    moqr_shards_t       *shards;
    moqr_cli_snapshot_t *snap;   /* metrics rows: lanes publish, main renders */
    uint32_t             lanes;
    moqr_obs_labels_t    labels[MOQR_CLI_MAX_LANES];
    unsigned             seen_state_epoch[MOQR_CLI_MAX_LANES];
    unsigned             seen_trace_epoch[MOQR_CLI_MAX_LANES];
    uint64_t             pump_turns[MOQR_CLI_MAX_LANES];
    uint64_t             wake_pushes[MOQR_CLI_MAX_LANES];
} serve_lanes_ctx_t;

/* Lane i drives shard i: attach/reap this lane's connections against shard i's
 * binding, step the shard, then wake exactly the destination lanes whose
 * control mailboxes accepted a cross-shard push. Every session touched here is
 * owned by this lane, under this lane's lock domain. */
static int
relay_lanes_pump(moq_msquic_managed_t *m, moq_msquic_managed_lane_t *lane,
                 uint64_t now_us, void *vctx)
{
    serve_lanes_ctx_t *ctx = vctx;
    uint32_t shard = moq_msquic_lane_index(lane);
    if (shard >= moqr_shards_count(ctx->shards)) {
        return 0;   /* adapter clamped lanes below shards: nothing to drive */
    }
    moqr_bind_t *bind = moqr_shards_bind(ctx->shards, (uint16_t)shard);
    for (moq_msquic_managed_conn_t *conn = moq_msquic_lane_next_conn(lane,
                                                                     NULL);
         conn != NULL; conn = moq_msquic_lane_next_conn(lane, conn)) {
        void *tag = moq_msquic_managed_conn_user(conn);
        if (tag == RELAY_CONN_DEAD) {
            continue;
        }
        moq_session_t *s = moq_msquic_managed_conn_session(conn);
        if (s == NULL) {
            continue;
        }
        if (tag == NULL) {
            if (moqr_bind_conn_open(bind, s,
                                    moq_msquic_managed_conn_negotiated_version(conn)) == MOQR_OK) {
                moq_msquic_managed_conn_set_user(conn, RELAY_CONN_OPENED);
            } else {
                moq_msquic_managed_conn_set_user(conn, RELAY_CONN_DEAD);
                moq_msquic_managed_conn_close(conn, 0);
            }
        }
    }

    uint64_t mask = 0;
    moqr_result_t rc =
        moqr_shards_step_shard(ctx->shards, (uint16_t)shard, now_us, &mask);
    ctx->pump_turns[shard]++;
    for (uint32_t d = 0; mask != 0 && d < ctx->lanes; d++) {
        if (mask & (1ull << d)) {
            (void)moq_msquic_lane_wake(moq_msquic_managed_lane(m, d));
            ctx->wake_pushes[shard]++;
        }
    }
    if (rc != MOQR_OK) {
        /* Bind pump error or this shard's manager fail-stop: terminate the
         * whole facade cleanly rather than serve over a lost observation. */
        fprintf(stderr, "MOQ5 Relay: shard %u step failed (%d) — stopping\n",
                shard, (int)rc);
        return 1;
    }
    /* A binding this step detached must be retired now: nothing else wakes
     * this lane on its behalf. */
    if (!moqr_relay_reap_pass(bind, lane, NULL)) {
        fprintf(stderr, "MOQ5 Relay: shard %u connection retirement failed "
                        "— stopping\n", shard);
        return 1;
    }

    /* Operator dumps: THIS lane renders its own route + journal dumps once
     * per new epoch (safe here — its core/mgr/trace are read only inside
     * its own pump), and PUBLISHES its metrics row for the coordinator —
     * per-lane Prometheus documents are gone; the main thread renders one
     * multi-shard exposition once every row carries the newest epoch. */
    char lbl[32];
    unsigned se = atomic_load(&g_dump_state_epoch);
    if (ctx->seen_state_epoch[shard] != se) {
        ctx->seen_state_epoch[shard] = se;
        snprintf(lbl, sizeof(lbl), "shard %u routes", shard);
        dump_emit(lbl, dump_routes_fn,
                  moqr_shards_core(ctx->shards, (uint16_t)shard));
        dump_journal_ctx_t jc = { ctx->shards, (uint16_t)shard };
        snprintf(lbl, sizeof(lbl), "shard %u journal", shard);
        dump_emit(lbl, dump_journal_fn, &jc);
        moqr_cli_snapshot_stats_t st;
        memset(&st, 0, sizeof(st));
        moqr_core_get_stats(moqr_shards_core(ctx->shards, (uint16_t)shard),
                            &st.core);
        moqr_bind_get_stats(bind, &st.bind);
        /* A refused shard snapshot (poisoned state) publishes as INVALID —
         * the row still completes the epoch, and the coordinator suppresses
         * the whole document rather than aggregating zeroed stand-ins. */
        st.shard_stats_valid =
            moqr_shards_get_stats(ctx->shards, (uint16_t)shard, &st.shard) ==
            MOQR_OK;
        st.lane_wakes = ctx->wake_pushes[shard];
#ifdef MOQR_BIND_TESTING
        /* Verify build: the LIVE blocked-reason aggregate for this lane,
         * read on the owning lane thread while connections are still up and
         * parked state is intact — a post-detach read would report zeros. */
        st.blocked_valid =
            moqr_bind_debug_blocked_aggregate(bind, &st.blocked) == MOQR_OK;
        /* Stranding diagnostics (lane-owned, NON-authoritative — the
         * coordinated RELAY_BLOCKED_V0 record is unchanged): per ACTIVE
         * conn slot over the RESOLVED capacity, the downstream pool
         * occupancy, the delivery-scheduler state, and the three historical
         * blockage counters — the blocked connection is IDENTIFIED by its
         * counters (the parser requires exactly one bind_sg_total > 0 row),
         * never inferred. Interpretation of the occ/ready split is
         * PRE-REGISTERED in moqr_cli_sg_diagnose (blockedstats.h): combined
         * with seal presence over an interval-complete SEALLOG window it
         * yields exactly one of NOT_APPLIED / NO_RELEASE / PASS_INCOMPLETE
         * / NOT_REARMED / DOORBELL_UNCONSUMED — never a post-hoc story. */
        for (uint32_t sl = 0; sl < moqr_bind_debug_max_conns(bind); sl++) {
            uint32_t occ = moqr_bind_debug_conn_open_sgs(bind, sl);
            bool rd = false, pk = false;
            uint8_t reason = 0;
            moqr_bind_debug_dl_state(bind, sl, &rd, &pk, &reason);
            uint64_t bc[3];
            moqr_bind_debug_conn_blocked_counts(bind, sl, bc);
            if (occ == 0 && !rd && !pk && bc[0] == 0 && bc[1] == 0 &&
                bc[2] == 0) {
                continue;   /* quiet slot: not worth a line */
            }
            /* Demand attribution joins the blocked conn to its SEALLOG
             * seals exactly: the bind captured the refused delivery's
             * TRACK (first refusal latches; a second track — a second
             * demand — flags ambiguity), and the shard's demand table
             * resolves track → demand id. 0 stays 0 (a local track has no
             * demand) and the acceptance fails closed on it. */
            uint64_t bsd = 0;
            bool bsa = false;
            {
                uint64_t btr = 0, btg = 0;
                if (moqr_bind_debug_conn_bind_sg_track(bind, sl, &btr, &btg,
                                                       &bsa)) {
                    bsd = moqr_shards_debug_track_demand(ctx->shards, shard,
                                                         btr, btg);
                }
            }
            moqr_cli_sgdiag_row_t dg = {
                .epoch = (uint64_t)se,
                .lane = shard,
                .slot = sl,
                .occ = occ,
                .ready = rd ? 1u : 0u,
                .parked = pk ? 1u : 0u,
                .reason = reason,
                .action_cap_total = bc[0],
                .session_sg_total = bc[1],
                .bind_sg_total = bc[2],
                .bind_sg_demand = bsd,
                .bind_sg_demand_ambiguous = bsa ? 1u : 0u,
            };
            char dline[MOQR_BLOCKED_ROW_MAX];
            if (moqr_cli_sgdiag_format(dline, sizeof(dline), &dg) > 0) {
                fprintf(stderr, "%s\n", dline);
            }
        }
        /* Destination SG_SEAL ingest evidence, oldest first (bounded ring;
         * seq is the lifetime ingest order on this lane). The ring keeps only
         * the newest window, so lifetime completeness is impossible after it
         * wraps: emit a RELAY_SEALMETA header FIRST (always, even at zero
         * events) pinning the window's lower edge and the lifetime total, so
         * a later dump can prove interval-completeness against a prior cursor
         * without every historical seal surviving. */
        {
            moqr_shards_seal_ev_t evs[32];
            uint64_t seal_total = 0;
            uint32_t n = moqr_shards_debug_seal_log(ctx->shards, shard, evs,
                                                    32, &seal_total);
            moqr_cli_seallog_meta_t sm = {
                .epoch = (uint64_t)se,
                .lane = shard,
                /* oldest retained seq; when nothing is retained the window is
                 * the empty interval [total, total). */
                .first_retained_seq = n > 0 ? evs[0].seq : seal_total,
                .lifetime_total = seal_total,
                .retained_count = n,
            };
            char mline[MOQR_BLOCKED_ROW_MAX];
            if (moqr_cli_seallog_meta_format(mline, sizeof(mline), &sm) > 0) {
                fprintf(stderr, "%s\n", mline);
            }
            for (uint32_t k = 0; k < n; k++) {
                moqr_cli_seallog_row_t sr = {
                    .epoch = (uint64_t)se,
                    .lane = shard,
                    .seq = evs[k].seq,
                    .total = seal_total,
                    .src = evs[k].src,
                    .demand = evs[k].demand_id,
                    .group_id = evs[k].group_id,
                    .subgroup_id = evs[k].subgroup_id,
                };
                char sline[MOQR_BLOCKED_ROW_MAX];
                if (moqr_cli_seallog_format(sline, sizeof(sline), &sr) > 0) {
                    fprintf(stderr, "%s\n", sline);
                }
            }
        }
#endif
        moqr_cli_snapshot_publish(ctx->snap, shard, &st, (uint64_t)se);
    }
    unsigned te = atomic_load(&g_dump_trace_epoch);
    if (ctx->seen_trace_epoch[shard] != te) {
        ctx->seen_trace_epoch[shard] = te;
        snprintf(lbl, sizeof(lbl), "shard %u trace", shard);
        dump_emit(lbl, dump_trace_fn,
                  moqr_shards_trace(ctx->shards, (uint16_t)shard));
    }
    return atomic_load(&g_stop) ? 1 : 0;
}

/* The coordinator's view of the newest requested metrics epoch (the same
 * lock-free counter the signal handler bumps and the lanes observe). */
static uint64_t
coord_epoch(void *unused)
{
    (void)unused;
    return (uint64_t)atomic_load(&g_dump_state_epoch);
}

#ifndef MOQR_BIND_TESTING
/* Emit one licensed exposition document to stderr under the banner (the
 * production moqr_cli_emit_fn). */
static void
coord_emit_stderr(void *ectx, const char *doc, size_t len, uint64_t epoch)
{
    (void)ectx;
    (void)len;
    fprintf(stderr, "\n===== MOQ5 Relay metrics (epoch %llu) =====\n%s\n",
            (unsigned long long)epoch, doc);
}

/* One coordinator attempt at the outstanding metrics dump, entirely
 * through the CLI-private moqr_cli_coord_dump seam (which, by
 * construction, is never handed the shard runtime — the rows are all it
 * can read). Returns the epoch rendered (or suppressed-with-diagnostic),
 * else `rendered` when the newest epoch is still incomplete — the caller
 * retries on its next wait tick; *incomplete_told and *fail_told de-spam
 * the two retryable diagnostics (one line per epoch each). */
static unsigned
coord_try_dump(const serve_lanes_ctx_t *ctx, moqr_cli_snapshot_t *snap,
               unsigned rendered, unsigned *incomplete_told,
               unsigned *fail_told)
{
    uint64_t epoch = 0;
    moqr_result_t rc =
        moqr_cli_coord_dump(snap, ctx->labels, coord_epoch, NULL,
                            coord_emit_stderr, NULL, &epoch);
    if (rc == MOQR_OK) {
        rendered = (unsigned)epoch;
    } else if (rc == MOQR_ERR_INVAL) {
        /* Suppression: a lane published a refused (poisoned) shard
         * snapshot, or an internal-entity exclusion went negative.
         * Diagnose and consume the epoch — never floored, never a zeroed
         * stand-in, never spun on. */
        fprintf(stderr,
                "MOQ5 Relay: metrics epoch %llu suppressed — a lane's shard "
                "snapshot was refused or an internal-entity exclusion went "
                "negative (snapshot contradicts itself)\n",
                (unsigned long long)epoch);
        rendered = (unsigned)epoch;
    } else if (rc == MOQR_ERR_WOULD_BLOCK) {
        if (*incomplete_told != (unsigned)epoch) {
            fprintf(stderr,
                    "MOQ5 Relay: metrics epoch %llu incomplete — waiting "
                    "for every lane to publish\n",
                    (unsigned long long)epoch);
            *incomplete_told = (unsigned)epoch;
        }
    } else if (*fail_told != (unsigned)epoch) {
        /* Unexpected but retryable (e.g. a transient allocation failure):
         * the epoch stays outstanding and retries on the wait cadence —
         * diagnosed once per epoch, never a silent spin. */
        fprintf(stderr,
                "MOQ5 Relay: metrics epoch %llu dump failed (%d) — "
                "retrying\n",
                (unsigned long long)epoch, (int)rc);
        *fail_told = (unsigned)epoch;
    }
    return rendered;
}
#else  /* MOQR_BIND_TESTING: moq-relay-verify emits RELAY_BLOCKED_V0 instead */

/* The blocked-document producer: serialize one coherent same-epoch row set
 * (already collected under the snapshot mutexes) into K RELAY_BLOCKED_V0
 * lines, lane 0..K-1 ascending. A lane whose LIVE aggregate failed
 * (blocked_valid == false) emits an explicit refusal row that invalidates the
 * whole epoch for the gauntlet parser — never valid-looking zeros. Produce
 * only; the render cycle's final epoch re-read licenses emission. */
typedef struct blocked_doc {
    uint32_t lanes;
    size_t   len;
    char    *buf;    /* lanes * ~256 B, caller-allocated */
    size_t   cap;
} blocked_doc_t;

static moqr_result_t
blocked_produce(void *pctx, const moqr_cli_snapshot_stats_t *rows,
                uint64_t epoch)
{
    blocked_doc_t *d = pctx;
    d->len = 0;
    for (uint32_t i = 0; i < d->lanes; i++) {
        char line[MOQR_BLOCKED_ROW_MAX];   /* fits any legal all-max row */
        int w;
        if (rows[i].blocked_valid) {
            const moqr_bind_blocked_agg_t *a = &rows[i].blocked;
            moqr_cli_blocked_row_t r = {
                .epoch = epoch,
                .lane = i,
                .live_conns = a->live_conns,
                .conns_action_cap = a->conns_action_cap,
                .conns_session_sg = a->conns_session_sg,
                .conns_bind_sg = a->conns_bind_sg,
                .action_cap_total = a->action_cap_total,
                .session_sg_total = a->session_sg_total,
                .bind_sg_total = a->bind_sg_total,
                .parked_action_cap = a->parked_action_cap,
                .parked_session_sg = a->parked_session_sg,
            };
            w = moqr_cli_blocked_format(line, sizeof(line), &r);
            if (w < 0) {
                /* A legal row cannot overflow MOQR_BLOCKED_ROW_MAX; if it
                 * somehow does, degrade this lane to an explicit refusal
                 * (invalidates the epoch) rather than stalling every epoch. */
                w = moqr_cli_blocked_format_refused(line, sizeof(line), epoch,
                                                    i);
            }
        } else {
            w = moqr_cli_blocked_format_refused(line, sizeof(line), epoch, i);
        }
        if (w < 0 || d->len + (size_t)w + 1 >= d->cap) {
            return MOQR_ERR_INTERNAL;   /* fail closed: no partial document */
        }
        memcpy(d->buf + d->len, line, (size_t)w);
        d->len += (size_t)w;
        d->buf[d->len++] = '\n';
    }
    d->buf[d->len] = '\0';
    return MOQR_OK;
}

/* One verify-build coordinator attempt: the same collect -> produce ->
 * final-epoch-reread cycle, but the producer is the blocked document. Emits
 * exactly once, only when the cycle licenses it. */
static unsigned
coord_try_dump_blocked(const serve_lanes_ctx_t *ctx, moqr_cli_snapshot_t *snap,
                       unsigned rendered, unsigned *incomplete_told,
                       unsigned *fail_told)
{
    moqr_cli_snapshot_stats_t *rows =
        calloc(ctx->lanes, sizeof(*rows));
    blocked_doc_t doc;
    doc.lanes = ctx->lanes;
    /* Room for the newline after each row plus the document NUL — sized off
     * the shared per-row maximum so a full lane set of all-max rows fits. */
    doc.cap = (size_t)ctx->lanes * (MOQR_BLOCKED_ROW_MAX + 1u) + 1u;
    doc.buf = malloc(doc.cap);
    if (rows == NULL || doc.buf == NULL) {
        free(rows);
        free(doc.buf);
        return rendered;   /* transient; retried on the next wait tick */
    }
    uint64_t epoch = 0;
    moqr_result_t rc = moqr_cli_snapshot_render(
        snap, coord_epoch, NULL, rows, blocked_produce, &doc, &epoch);
    if (rc == MOQR_OK) {
        fprintf(stderr,
                "\n===== moq-relay-verify blocked (epoch %llu) =====\n%s\n",
                (unsigned long long)epoch, doc.buf);
        rendered = (unsigned)epoch;
    } else if (rc == MOQR_ERR_INVAL) {
        /* A lane published a refused shard snapshot: suppress and consume. */
        fprintf(stderr,
                "moq-relay-verify: blocked epoch %llu suppressed — a lane's "
                "shard snapshot was refused\n",
                (unsigned long long)epoch);
        rendered = (unsigned)epoch;
    } else if (rc == MOQR_ERR_WOULD_BLOCK) {
        if (*incomplete_told != (unsigned)epoch) {
            fprintf(stderr,
                    "moq-relay-verify: blocked epoch %llu incomplete — "
                    "waiting for every lane to publish\n",
                    (unsigned long long)epoch);
            *incomplete_told = (unsigned)epoch;
        }
    } else if (*fail_told != (unsigned)epoch) {
        fprintf(stderr,
                "moq-relay-verify: blocked epoch %llu dump failed (%d) — "
                "retrying\n",
                (unsigned long long)epoch, (int)rc);
        *fail_told = (unsigned)epoch;
    }
    free(rows);
    free(doc.buf);
    return rendered;
}
#endif  /* MOQR_BIND_TESTING */

static int
cmd_serve_lanes(const moqr_cli_config_t *cfg)
{
    if (cfg->cert[0] == '\0' || cfg->key[0] == '\0') {
        fprintf(stderr, "serve requires listener.cert and listener.key\n");
        return 2;
    }

    /* One shard per lane; each shard = {core, bind, trace}. The CLI-private
     * serve seam builds the shard config (production admission = lanes > 1,
     * the strict per-lane bind clamp) AND the facade admission cap from the
     * ONE builder `capacity` also consumes — so the ceiling described is
     * exactly the config running here. live_visibility = true: lanes step
     * concurrently with no shared round to advance. */
    moqr_shards_cfg_t scfg;
    uint32_t serve_max_conns = 0;
    if (moqr_cli_serve_compose(cfg, moq_alloc_default(), &scfg,
                               &serve_max_conns) != MOQR_OK) {
        fprintf(stderr, "serve: config refused (budgets vs lanes)\n");
        return 2;
    }

    moqr_shards_t *shards = NULL;
    if (moqr_shards_create(&scfg, &shards) != MOQR_OK) {
        fprintf(stderr, "relay shard runtime create failed\n");
        return 1;
    }

    serve_lanes_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        moqr_shards_destroy(shards);
        return 1;
    }
    /* Snapshot-row storage (counted in cli_runtime_bytes). Publication and
     * rendering are wired by the observability slice; allocating the rows
     * here keeps the printed ceiling equal to what serve really requests. */
    moqr_cli_snapshot_t snap;
    if (moqr_cli_snapshot_init(&snap, cfg->lanes, moq_alloc_default()) !=
        MOQR_OK) {
        free(ctx);
        moqr_shards_destroy(shards);
        return 1;
    }
    ctx->shards = shards;
    ctx->snap = &snap;
    ctx->lanes = cfg->lanes;
    for (uint32_t i = 0; i < cfg->lanes; i++) {
        ctx->labels[i] = (moqr_obs_labels_t){ .shard = (uint16_t)i,
                                              .transport = "msquic",
                                              .version = cfg->alpn_set };
    }


    moq_msquic_managed_cfg_t tcfg;
    moq_msquic_managed_cfg_init_sized(&tcfg, sizeof(tcfg));
    tcfg.alloc = moq_alloc_default();
    tcfg.perspective = MOQ_PERSPECTIVE_SERVER;
    tcfg.host = cfg->host;
    tcfg.port = (uint16_t)cfg->port;
    tcfg.cert_path = cfg->cert;
    tcfg.key_path = cfg->key;
    tcfg.insecure_skip_verify = cfg->insecure_skip_verify;
    tcfg.send_request_capacity = true;
    tcfg.initial_request_capacity = 1024;
    tcfg.streaming_objects = true;
    moqr_cli_apply_versions(cfg, &tcfg);
    tcfg.lane_count = cfg->lanes;
    /* Facade admission cap: the seam's usable-bindings-per-shard x lanes
     * rule (the strict per-lane clamp, not lanes x max_bindings). */
    tcfg.max_connections = serve_max_conns;
    tcfg.on_lane_pump = relay_lanes_pump;
    tcfg.on_lane_pump_user = ctx;
#ifdef MOQR_VERIFY_SEAM
    /* Blocked-scenario seam: constrain every relay-side session's outgoing
     * subgroup pool (SESSION_SG induction; the bind override rode the shard
     * builder above). Echoed so the gauntlet asserts the constraint applied
     * rather than trusting the environment reached the process. */
    tcfg.max_open_subgroups = moqr_cli_verify_session_max_sgs();
    if (moqr_cli_verify_bind_max_sgs() != 0 ||
        moqr_cli_verify_session_max_sgs() != 0) {
        printf(MOQR_SEAM_NAME ": seam bind_max_open_subgroups=%u "
               "session_max_open_subgroups=%u (0=default)\n",
               moqr_cli_verify_bind_max_sgs(),
               moqr_cli_verify_session_max_sgs());
    }
#endif

    moq_msquic_managed_t *t = NULL;
    if (moq_msquic_managed_create(&tcfg, &t) != MOQ_OK) {
        fprintf(stderr, "transport create failed (port %d)\n", cfg->port);
        moqr_cli_snapshot_destroy(&snap);
        moqr_shards_destroy(shards);
        free(ctx);
        return 1;
    }

    serve_install_signals(on_dump_signal_lanes);
    printf("MOQ5 Relay: listening on %s:%d (%s), %u lanes\n", cfg->host,
           cfg->port, cfg->alpn_set, cfg->lanes);
    serve_print_run_config(&scfg);
    if (print_capacity(cfg, sizeof(serve_lanes_ctx_t)) != 0) {
        /* An undescribable ceiling never serves. */
        moq_msquic_managed_destroy(t);
        moqr_cli_snapshot_destroy(&snap);
        moqr_shards_destroy(shards);
        free(ctx);
        return 2;
    }
    printf("signals: SIGUSR1 -> metrics + route dump, "
           "SIGUSR2 -> trace JSONL (per shard, to stderr)\n");
    fflush(stdout);
    /* The coordinator loop: between waits, service any outstanding metrics
     * epoch by copying the lanes' published rows (their dump-only mutexes
     * are the ONLY lane state touched — never a live lane's core, bind,
     * journal, or trace) and rendering one multi-shard exposition once the
     * newest requested epoch is complete on every row. An IDLE lane's pump
     * never runs on its own, so an outstanding dump epoch also wakes every
     * lane — each pump then renders its route/journal (or trace) dump and
     * publishes its row; redundant wakes are harmless by design. */
    unsigned rendered_epoch = 0;
    unsigned incomplete_told = 0;
    unsigned fail_told = 0;
    unsigned woken_trace = 0;
    while (!atomic_load(&g_stop)) {
        if (moq_msquic_managed_wait(t, 500 * 1000) == MOQ_ERR_CLOSED) {
            break;
        }
        bool state_owed =
            atomic_load(&g_dump_state_epoch) != rendered_epoch;
        unsigned te = atomic_load(&g_dump_trace_epoch);
        if (state_owed || te != woken_trace) {
            woken_trace = te;
            for (uint32_t d = 0; d < cfg->lanes; d++) {
                (void)moq_msquic_lane_wake(moq_msquic_managed_lane(t, d));
            }
        }
        if (state_owed) {
#ifdef MOQR_BIND_TESTING
            rendered_epoch = coord_try_dump_blocked(
                ctx, &snap, rendered_epoch, &incomplete_told, &fail_told);
#else
            rendered_epoch = coord_try_dump(ctx, &snap, rendered_epoch,
                                            &incomplete_told, &fail_told);
#endif
        }
    }
    (void)moq_msquic_managed_stop(t);   /* joins every lane; safe to read now */

    /* Per-lane attribution rows: the adapter's doorbell counters joined with
     * the same lane's shard counters, one stable machine-readable line per
     * lane (post-join, coordinator thread — both getters are legal here: an
     * external thread snapshots the adapter stats under the lane lock). A
     * refused getter emits an explicit REFUSAL record, never zeros. */
    for (uint32_t i = 0; i < cfg->lanes; i++) {
        char line[2048];   /* a legal all-max V3 row is ~1.5 KiB */
        moq_msquic_lane_stats_t ad;
        moqr_shards_stats_t ss;
        memset(&ad, 0, sizeof(ad));
        ad.struct_size = (uint32_t)sizeof(ad);
        bool ad_ok = moq_msquic_lane_get_stats(moq_msquic_managed_lane(t, i),
                                               &ad, sizeof(ad)) == MOQ_OK;
        bool sh_ok =
            moqr_shards_get_stats(shards, (uint16_t)i, &ss) == MOQR_OK;
        int len;
        if (!ad_ok || !sh_ok) {
            len = moqr_cli_lane_stats_format_refused(
                line, sizeof(line), i, !ad_ok ? "adapter" : "shard");
        } else {
            moqr_cli_lane_stats_row_t row;
            memset(&row, 0, sizeof(row));
            row.lane = i;
            row.wakes_same_lane = ad.wakes_same_lane;
            row.wakes_cross_lane = ad.wakes_cross_lane;
            row.wakes_external = ad.wakes_external;
            row.wakes_coalesced = ad.wakes_coalesced;
            row.pump_sweeps = ad.pump_sweeps;
            row.deadline_sweeps = ad.deadline_sweeps;
            row.idle_cap_wakes = ad.idle_cap_wakes;
            row.wake_to_pump_max_us = ad.wake_to_pump_max_us;
            row.wake_to_pump_total_us = ad.wake_to_pump_total_us;
            row.wake_to_pump_samples = ad.wake_to_pump_samples;
            row.service_passes = ad.service_passes;
            row.flush_sends = ad.flush_sends;
            row.flush_bytes = ad.flush_bytes;
            row.pump_turns = ss.pump_turns;
            row.pump_messages = ss.pump_messages;
            row.pump_bytes = ss.pump_bytes;
            row.wake_requests_push = ss.wake_requests_push;
            row.wake_requests_credit = ss.wake_requests_credit;
            row.wake_requests_local = ss.wake_requests_local;
            row.enq_demand = ss.enqueued[MOQR_SHARDS_MSG_DEMAND];
            row.enq_undemand = ss.enqueued[MOQR_SHARDS_MSG_UNDEMAND];
            row.enq_done = ss.enqueued[MOQR_SHARDS_MSG_DONE];
            row.enq_ack = ss.enqueued[MOQR_SHARDS_MSG_ACK];
            row.enq_obj = ss.enqueued[MOQR_SHARDS_MSG_OBJ];
            row.enq_obj_open = ss.enqueued[MOQR_SHARDS_MSG_OBJ_OPEN];
            row.enq_obj_chunk = ss.enqueued[MOQR_SHARDS_MSG_OBJ_CHUNK];
            row.enq_obj_end = ss.enqueued[MOQR_SHARDS_MSG_OBJ_END];
            row.enq_obj_reset = ss.enqueued[MOQR_SHARDS_MSG_OBJ_RESET];
            row.enq_grp_reset = ss.enqueued[MOQR_SHARDS_MSG_GRP_RESET];
            row.enq_grp_evict = ss.enqueued[MOQR_SHARDS_MSG_GRP_EVICT];
            row.enq_sg_seal = ss.enqueued[MOQR_SHARDS_MSG_SG_SEAL];
            row.channel_entries_hwm = ss.channel_entries_hwm;
            row.channel_bytes_hwm = ss.channel_bytes_hwm;
            row.turns_msg_budget = ss.turns_msg_budget;
            row.turns_byte_budget = ss.turns_byte_budget;
            row.turns_blocked = ss.turns_blocked;
            row.turns_drained = ss.turns_drained;
            row.turns_with_messages = ss.turns_with_messages;
            row.arb_class_refusals = ss.arb_class_refusals;
            len = moqr_cli_lane_stats_format(line, sizeof(line), &row);
        }
        if (len > 0 && (size_t)len < sizeof(line)) {
            printf("%s\n", line);
        } else {
            /* a truncated row must never masquerade as a valid record */
            printf(MOQR_LANE_STATS_PREFIX ",lane=%u,refused=adapter\n", i);
        }
    }
    /* Per-directed-pair rows, in the deterministic record order the parser
     * enforces (src-major, ascending, self skipped): exactly K*(K-1) rows
     * or a refusal — a partial pair record must fail closed downstream. */
    for (uint32_t src = 0; src < cfg->lanes; src++) {
        for (uint32_t dst = 0; dst < cfg->lanes; dst++) {
            if (dst == src) {
                continue;
            }
            char pline[512];
            moqr_shards_pair_stats_t pst;
            int plen = -1;
            if (moqr_shards_get_pair_stats(shards, (uint16_t)src,
                                           (uint16_t)dst, &pst,
                                           sizeof(pst)) == MOQR_OK) {
                moqr_cli_pair_stats_row_t pr;
                memset(&pr, 0, sizeof(pr));
                pr.src = src;
                pr.dst = dst;
                pr.data_messages = pst.data_messages;
                pr.data_bytes = pst.data_bytes;
                pr.control_messages = pst.control_messages;
                pr.refused_entries = pst.refused_entries;
                pr.refused_bytes = pst.refused_bytes;
                plen = moqr_cli_pair_stats_format(pline, sizeof(pline), &pr);
            }
            if (plen > 0 && (size_t)plen < sizeof(pline)) {
                printf("%s\n", pline);
            } else {
                printf(MOQR_PAIR_STATS_PREFIX
                       ",src=%u,dst=%u,refused=shard\n", src, dst);
            }
        }
    }

    uint64_t sum_ingested = 0, sum_delivered = 0, sum_sesserr = 0;
    uint32_t sum_conns = 0, sum_tracks = 0;
    for (uint32_t i = 0; i < cfg->lanes; i++) {
        moqr_bind_stats_t bs;
        moqr_bind_get_stats(moqr_shards_bind(shards, (uint16_t)i), &bs);
        moqr_core_stats_t cs;
        moqr_core_get_stats(moqr_shards_core(shards, (uint16_t)i), &cs);
        /* Post-join direct snapshot: every lane has joined, so the shard
         * stats are readable here without requesting another epoch. A
         * refused (poisoned) snapshot is diagnosed, never printed as
         * zeros-that-look-valid. */
        moqr_shards_stats_t ss;
        bool ss_ok = moqr_shards_get_stats(shards, (uint16_t)i, &ss) ==
                     MOQR_OK;
        if (ss_ok) {
            printf("MOQ5 Relay: shard %u — conns %u, tracks %u, ingested %"
                   PRIu64 ", delivered %" PRIu64 ", session errors %" PRIu64
                   ", pump turns %" PRIu64 ", wakes %" PRIu64
                   " (requests: push %" PRIu64 ", credit %" PRIu64
                   ", local %" PRIu64 ")\n",
                   i, bs.conns, cs.tracks, cs.ingested_total,
                   cs.delivered_total, bs.session_errors, ctx->pump_turns[i],
                   ctx->wake_pushes[i], ss.wake_requests_push,
                   ss.wake_requests_credit, ss.wake_requests_local);
        } else {
            printf("MOQ5 Relay: shard %u — conns %u, tracks %u, ingested %"
                   PRIu64 ", delivered %" PRIu64 ", session errors %" PRIu64
                   ", pump turns %" PRIu64 ", wakes %" PRIu64
                   " (shard stats refused: poisoned snapshot)\n",
                   i, bs.conns, cs.tracks, cs.ingested_total,
                   cs.delivered_total, bs.session_errors, ctx->pump_turns[i],
                   ctx->wake_pushes[i]);
        }
        sum_conns += bs.conns;
        sum_tracks += cs.tracks;
        sum_ingested += cs.ingested_total;
        sum_delivered += cs.delivered_total;
        sum_sesserr += bs.session_errors;
    }
    printf("MOQ5 Relay: stopping — total conns %u, tracks %u, ingested %" PRIu64
           ", delivered %" PRIu64 ", session errors %" PRIu64 "\n",
           sum_conns, sum_tracks, sum_ingested, sum_delivered, sum_sesserr);

    moq_msquic_managed_destroy(t);
    moqr_cli_snapshot_destroy(&snap);
    moqr_shards_destroy(shards);   /* owns per-shard bind/core/trace teardown */
    free(ctx);
    return 0;
}

int
main(int argc, char **argv)
{
    moqr_cli_args_t args;

    if (!moqr_cli_args_parse(argc, argv, &args)) {
        fprintf(stderr, "MOQ5 Relay: %s\n", args.error);
        moqr_cli_print_try_help(stderr);
        return 2;
    }
    /* Informational commands are answered from argv alone — before the
     * verify-seam environment, the configuration file, the capacity preflight
     * and any allocation. `--help` has to work on a host where none of those
     * are in order. */
    if (moqr_cli_cmd_is_informational(args.cmd)) {
        if (args.cmd == MOQR_CLI_CMD_VERSION) {
            moqr_cli_print_version(stdout);
        } else {
            moqr_cli_print_help(stdout, args.cmd);
        }
        return 0;
    }

    const char *cmd = args.cmd == MOQR_CLI_CMD_SERVE ? "serve" : "capacity";
    const char *path = args.config;

#ifdef MOQR_VERIFY_SEAM
    /* Blocked-scenario constraint seam: read + validate BEFORE the config,
     * so a junk value refuses here rather than serving unconstrained (the
     * scenario's acceptance would then fail by timeout, not by diagnosis). */
    {
        char verr[192];
        if (moqr_cli_verify_env_load(verr, sizeof(verr)) != MOQR_OK) {
            fprintf(stderr, MOQR_SEAM_NAME ": %s\n", verr);
            return 2;
        }
    }
#endif

    moqr_cli_config_t cfg;
    char err[128];
    if (moqr_cli_config_load(path, &cfg, err, sizeof(err)) != MOQR_OK) {
        fprintf(stderr, "config error: %s\n", err);
        return 2;
    }

#ifdef MOQR_VERIFY_SEAM
    /* The seam lives in the multi-lane path (the shard-config builder and,
     * in the verify build, the lanes coordinator that emits
     * RELAY_BLOCKED_V0). A constrained single-lane serve would silently
     * ignore the bind override — refuse. */
    if (cfg.lanes <= 1 && (moqr_cli_verify_bind_max_sgs() != 0 ||
                           moqr_cli_verify_session_max_sgs() != 0)) {
        fprintf(stderr, MOQR_SEAM_NAME ": MOQR_VERIFY_*_MAX_OPEN_SUBGROUPS "
                        "requires listener.lanes > 1\n");
        return 2;
    }
#endif

    /* Full capacity preflight ahead of EVERY command: semantic resolution
     * AND ceiling arithmetic both complete here, so an invalid or
     * undescribable config refuses identically for capacity and both serve
     * paths — before any runtime allocation, listener start, or "listening"
     * banner. The in-command print re-renders the same pure function. */
    {
        moqr_cli_capacity_t pre;
        size_t ctxb = cfg.lanes > 1 ? sizeof(serve_lanes_ctx_t) : 0;
        if (moqr_cli_describe_capacity(&cfg, moq_alloc_default(), ctxb,
                                       &pre) != MOQR_OK) {
            fprintf(stderr,
                    "config refused: cross-field validation or ceiling "
                    "arithmetic failed (e.g. demand_channel_bytes below one "
                    "resolved log record, or an overflowing budget)\n");
            return 2;
        }
    }

    if (strcmp(cmd, "capacity") == 0) {
        /* Valid at every lane count: lanes>1 uses the SAME shard-config
         * builder serve consumes, so the printed ceiling is the config the
         * process would actually run. */
        return print_capacity(&cfg, cfg.lanes > 1
                                        ? sizeof(serve_lanes_ctx_t)
                                        : 0) != 0
                   ? 2
                   : 0;
    }
    return cfg.lanes > 1 ? cmd_serve_lanes(&cfg) : cmd_serve(&cfg);
}

#ifdef MOQR_PUMP_TESTING
/* Test-only accessors: hand the REAL production lane pumps and their context
 * builders to a deterministic test, and drive the REAL signal handler. The
 * test links this translation unit with main renamed away; nothing here is
 * compiled into a shipped binary. */
moq_msquic_lane_pump_fn moqr_test_single_pump(void) { return relay_lane_pump; }
moq_msquic_lane_pump_fn moqr_test_lanes_pump(void) { return relay_lanes_pump; }

void *moqr_test_mk_serve_ctx(moqr_bind_t *bind, moqr_core_t *core,
                             moqr_trace_t *trace)
{
    serve_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (ctx != NULL) {
        ctx->bind = bind;
        ctx->core = core;
        ctx->trace = trace;
    }
    return ctx;
}

void *moqr_test_mk_lanes_ctx(moqr_shards_t *shards, uint32_t lanes)
{
    serve_lanes_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (ctx != NULL) {
        ctx->shards = shards;
        ctx->lanes = lanes;
    }
    return ctx;
}

uint64_t moqr_test_lanes_ctx_pump_turns(void *vctx, uint32_t lane)
{
    serve_lanes_ctx_t *ctx = vctx;

    return lane < MOQR_CLI_MAX_LANES ? ctx->pump_turns[lane] : 0;
}

uint64_t moqr_test_lanes_ctx_wake_pushes(void *vctx, uint32_t lane)
{
    serve_lanes_ctx_t *ctx = vctx;

    return lane < MOQR_CLI_MAX_LANES ? ctx->wake_pushes[lane] : 0;
}

void moqr_test_raise_stop(void) { on_signal(SIGTERM); }
void moqr_test_clear_stop(void) { atomic_store(&g_stop, 0); }
#endif /* MOQR_PUMP_TESTING */
