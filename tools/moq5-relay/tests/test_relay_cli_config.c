/* Config parser tests: strict schema, loud failures, capacity-only path. */

#include "../cli/config.h"
#include "../cli/snapshot.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../tests/unit/test_support.h"

static moqr_result_t
parse(const char *json, moqr_cli_config_t *out, char *err, size_t errlen)
{
    return moqr_cli_config_parse(json, strlen(json), out, err, errlen);
}

/* -- the metrics-snapshot publish/collect protocol --------------------------- */

/* A fixed "newest requested epoch" source. */
static uint64_t
fixed_epoch(void *ctx)
{
    return *(const uint64_t *)ctx;
}

/* A scripted epoch source: returns vals[] in call order (saturating at the
 * last value) — the deterministic injection seam that requests E+1 exactly
 * between the coordinator's copy and its re-read. */
typedef struct epoch_seq {
    uint64_t vals[8];
    int      idx;
    int      count;
} epoch_seq_t;

static uint64_t
seq_epoch(void *ctx)
{
    epoch_seq_t *e = ctx;
    uint64_t v = e->vals[e->idx];
    if (e->idx + 1 < e->count) {
        e->idx++;
    }
    return v;
}

/* A cycling epoch source: repeats vals[0..count-1] forever — models a
 * request storm where every produced document is obsoleted post-production
 * yet each fresh collect still finds a matching (stale) target. */
typedef struct epoch_cycle {
    uint64_t vals[4];
    int      count;
    int      idx;
} epoch_cycle_t;

static uint64_t
cycle_epoch(void *ctx)
{
    epoch_cycle_t *e = ctx;
    uint64_t v = e->vals[e->idx % e->count];
    e->idx++;
    return v;
}

/* Newest-epoch-only coalescing: a set renders only when every row carries
 * exactly the newest requested epoch; a lagging or mixed set is "epoch
 * incomplete"; and an all-E set copied just before E+1 was requested is
 * DISCARDED, never handed out. */
static int
test_snapshot_protocol(void)
{
    int failures = 0;
    const moq_alloc_t *alloc = moq_alloc_default();
    moqr_cli_snapshot_t snap;
    MOQ_TEST_CHECK(moqr_cli_snapshot_init(&snap, 2, alloc) == MOQR_OK);

    moqr_cli_snapshot_stats_t st;
    memset(&st, 0, sizeof(st));
    moqr_cli_snapshot_stats_t rows[2];
    uint64_t e = 1, got = 0;

    /* Nothing published yet: epoch 1 is incomplete. */
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e, rows,
                                             &got) == MOQR_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_U64(got, 1);

    /* One lane published: still incomplete — never a partial render. */
    st.core.ingested_total = 7;
    st.lane_wakes = 3;
    moqr_cli_snapshot_publish(&snap, 0, &st, 1);
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e, rows,
                                             &got) == MOQR_ERR_WOULD_BLOCK);

    /* Both lanes at epoch 1: one coherent set. */
    st.core.ingested_total = 9;
    st.lane_wakes = 5;
    moqr_cli_snapshot_publish(&snap, 1, &st, 1);
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e, rows,
                                             &got) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(got, 1);
    MOQ_TEST_CHECK_EQ_U64(rows[0].core.ingested_total, 7);
    MOQ_TEST_CHECK_EQ_U64(rows[0].lane_wakes, 3);
    MOQ_TEST_CHECK_EQ_U64(rows[1].core.ingested_total, 9);

    /* Back-to-back epochs: 2 requested, one lane still at 1 — a MIXED set
     * never renders. */
    e = 2;
    st.core.ingested_total = 17;
    moqr_cli_snapshot_publish(&snap, 0, &st, 2);
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e, rows,
                                             &got) == MOQR_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_U64(got, 2);
    st.core.ingested_total = 19;
    moqr_cli_snapshot_publish(&snap, 1, &st, 2);
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e, rows,
                                             &got) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(got, 2);
    MOQ_TEST_CHECK_EQ_U64(rows[0].core.ingested_total, 17);

    /* The copy-then-request-then-render race, pinned by injection: every
     * row carries 2, and the scripted source reports 2 for the target read
     * but 3 for the post-copy re-read — the all-2 set MUST be discarded
     * and the retry reported incomplete against 3, never rendered as 2. */
    epoch_seq_t sq = { { 2, 3, 3, 3 }, 0, 4 };
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, seq_epoch, &sq, rows,
                                             &got) == MOQR_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_U64(got, 3);

    /* Once the lanes catch up to 3, the newest epoch renders. */
    st.core.ingested_total = 23;
    moqr_cli_snapshot_publish(&snap, 0, &st, 3);
    moqr_cli_snapshot_publish(&snap, 1, &st, 3);
    e = 3;
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e, rows,
                                             &got) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(got, 3);

    /* Argument hygiene. */
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(NULL, fixed_epoch, &e, rows,
                                             &got) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, NULL, NULL, rows,
                                             &got) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e, NULL,
                                             &got) == MOQR_ERR_INVAL);
    moqr_cli_snapshot_publish(&snap, 99, &st, 4);   /* bad lane: no-op */

    moqr_cli_snapshot_destroy(&snap);
    MOQ_TEST_PASS("snapshot_protocol");
    return failures;
}

/* A produce probe: counts serializations and echoes a scripted result —
 * proving what was produced, when, and that discarded output never leaks
 * to the caller as OK. */
typedef struct produce_probe {
    int           calls;
    uint64_t      last_epoch;
    moqr_result_t rc;
} produce_probe_t;

static moqr_result_t
probe_produce(void *ctx, const moqr_cli_snapshot_stats_t *rows,
              uint64_t epoch)
{
    (void)rows;
    produce_probe_t *p = ctx;
    p->calls++;
    p->last_epoch = epoch;
    return p->rc;
}

/* The emission gate: moqr_cli_snapshot_render licenses emission only when
 * NO newer epoch was requested up to the post-production re-read — a
 * request injected AFTER collection and serialization (the window collect()
 * alone cannot see) discards the produced document. */
static int
test_snapshot_render_gate(void)
{
    int failures = 0;
    const moq_alloc_t *alloc = moq_alloc_default();
    moqr_cli_snapshot_t snap;
    MOQ_TEST_CHECK(moqr_cli_snapshot_init(&snap, 2, alloc) == MOQR_OK);
    moqr_cli_snapshot_stats_t st;
    memset(&st, 0, sizeof(st));
    st.shard_stats_valid = true;
    moqr_cli_snapshot_stats_t rows[2];
    moqr_cli_snapshot_publish(&snap, 0, &st, 1);
    moqr_cli_snapshot_publish(&snap, 1, &st, 1);
    uint64_t e = 1, got = 0;

    /* Stable epoch: produced exactly once, emission licensed. */
    produce_probe_t pr = { 0, 0, MOQR_OK };
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, fixed_epoch, &e, rows,
                                            probe_produce, &pr, &got) ==
                   MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(got, 1);
    MOQ_TEST_CHECK_EQ_INT(pr.calls, 1);
    MOQ_TEST_CHECK_EQ_U64(pr.last_epoch, 1);

    /* Injection AFTER collection + serialization: the scripted source
     * reports 1 for both of collect's reads (a coherent all-1 set is
     * copied AND produced), then 2 on the post-production read — the
     * produced epoch-1 document must be DISCARDED, the retry reported
     * incomplete against 2, and OK-at-1 never returned. */
    epoch_seq_t sq = { { 1, 1, 2, 2, 2 }, 0, 5 };
    pr = (produce_probe_t){ 0, 0, MOQR_OK };
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, seq_epoch, &sq, rows,
                                            probe_produce, &pr, &got) ==
                   MOQR_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_U64(got, 2);
    MOQ_TEST_CHECK_EQ_INT(pr.calls, 1);   /* produced once, then discarded */
    MOQ_TEST_CHECK_EQ_U64(pr.last_epoch, 1);

    /* A producer failure (the renderer's suppression) returns verbatim. */
    pr = (produce_probe_t){ 0, 0, MOQR_ERR_INVAL };
    e = 1;
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, fixed_epoch, &e, rows,
                                            probe_produce, &pr, &got) ==
                   MOQR_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_U64(got, 1);
    MOQ_TEST_CHECK_EQ_INT(pr.calls, 1);

    /* A POISONED lane snapshot (shard stats refused, published invalid)
     * suppresses the complete epoch: INVAL, and the producer is never
     * called — zeroed stand-ins never render. */
    st.shard_stats_valid = false;
    moqr_cli_snapshot_publish(&snap, 1, &st, 2);
    st.shard_stats_valid = true;
    moqr_cli_snapshot_publish(&snap, 0, &st, 2);
    pr = (produce_probe_t){ 0, 0, MOQR_OK };
    e = 2;
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, fixed_epoch, &e, rows,
                                            probe_produce, &pr, &got) ==
                   MOQR_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_U64(got, 2);
    MOQ_TEST_CHECK_EQ_INT(pr.calls, 0);
    /* The lane recovering (a valid republish) un-poisons the next epoch. */
    moqr_cli_snapshot_publish(&snap, 1, &st, 3);
    moqr_cli_snapshot_publish(&snap, 0, &st, 3);
    e = 3;
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, fixed_epoch, &e, rows,
                                            probe_produce, &pr, &got) ==
                   MOQR_OK);
    MOQ_TEST_CHECK_EQ_INT(pr.calls, 1);

    /* Retry exhaustion under a request storm: every attempt collects a
     * coherent all-1 set (the cycling source's collect reads return 1),
     * produces it, and is then obsoleted by a post-production read of 7 —
     * when the retries run out, the reported epoch must be the NEWEST
     * OBSERVED request (7), never the last produced one (1). */
    moqr_cli_snapshot_publish(&snap, 0, &st, 1);
    moqr_cli_snapshot_publish(&snap, 1, &st, 1);
    epoch_cycle_t cy = { { 1, 1, 7, 0 }, 3, 0 };
    pr = (produce_probe_t){ 0, 0, MOQR_OK };
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, cycle_epoch, &cy, rows,
                                            probe_produce, &pr, &got) ==
                   MOQR_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_U64(got, 7);
    MOQ_TEST_CHECK(pr.calls >= 2);        /* it really cycled */
    MOQ_TEST_CHECK_EQ_U64(pr.last_epoch, 1);

    /* An incomplete epoch never produces anything. */
    pr = (produce_probe_t){ 0, 0, MOQR_OK };
    e = 5;
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, fixed_epoch, &e, rows,
                                            probe_produce, &pr, &got) ==
                   MOQR_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK_EQ_INT(pr.calls, 0);
    MOQ_TEST_CHECK(moqr_cli_snapshot_render(&snap, fixed_epoch, &e, rows,
                                            NULL, NULL, &got) ==
                   MOQR_ERR_INVAL);
    moqr_cli_snapshot_destroy(&snap);
    MOQ_TEST_PASS("snapshot_render_gate");
    return failures;
}

/* -- the snapshot protocol under real threads (TSan-covered) ----------------- */

typedef struct snap_lane {
    moqr_cli_snapshot_t *snap;
    uint32_t             lane;
    _Atomic unsigned    *epoch;   /* the coordinator's requested counter */
    _Atomic int         *stop;
    int                  errs;
} snap_lane_t;

/* Encode (lane, epoch) into a stat so the coordinator can prove every
 * accepted set is epoch-coherent per row. */
static uint64_t
snap_encode(uint32_t lane, unsigned epoch)
{
    return (uint64_t)lane * 1000000u + epoch;
}

static void *
snap_lane_run(void *arg)
{
    snap_lane_t *t = arg;
    unsigned last = 0;
    while (!atomic_load(t->stop)) {
        unsigned e = atomic_load(t->epoch);
        if (e != last) {
            moqr_cli_snapshot_stats_t st;
            memset(&st, 0, sizeof(st));
            st.shard_stats_valid = true;
            st.core.ingested_total = snap_encode(t->lane, e);
            moqr_cli_snapshot_publish(t->snap, t->lane, &st, e);
            last = e;
        }
        sched_yield();
    }
    return NULL;
}

static uint64_t
atomic_epoch_read(void *ctx)
{
    return (uint64_t)atomic_load((_Atomic unsigned *)ctx);
}

/* Lanes publish rows concurrently under their dump-only mutexes while the
 * coordinator collects: every accepted set is coherent (each row's payload
 * encodes exactly the accepted epoch — a mixed or stale render would break
 * the encoding), and the coordinator touches ONLY the snapshot rows. After
 * join, one direct read needs no epoch. */
static int
test_snapshot_protocol_mt(void)
{
    int failures = 0;
    enum { SNAP_LANES = 3, SNAP_EPOCHS = 64 };
    const moq_alloc_t *alloc = moq_alloc_default();
    moqr_cli_snapshot_t snap;
    MOQ_TEST_CHECK(moqr_cli_snapshot_init(&snap, SNAP_LANES, alloc) ==
                   MOQR_OK);
    _Atomic unsigned epoch = 0;
    _Atomic int stop = 0;
    snap_lane_t lanes[SNAP_LANES];
    pthread_t tids[SNAP_LANES];
    for (uint32_t i = 0; i < SNAP_LANES; i++) {
        lanes[i] = (snap_lane_t){ &snap, i, &epoch, &stop, 0 };
        MOQ_TEST_CHECK(pthread_create(&tids[i], NULL, snap_lane_run,
                                      &lanes[i]) == 0);
    }
    moqr_cli_snapshot_stats_t rows[SNAP_LANES];
    int rendered = 0;
    for (unsigned e = 1; e <= SNAP_EPOCHS; e++) {
        atomic_store(&epoch, e);
        for (int spin = 0; spin < 2000000; spin++) {
            uint64_t got = 0;
            moqr_result_t rc = moqr_cli_snapshot_collect(
                &snap, atomic_epoch_read, &epoch, rows, &got);
            if (rc == MOQR_OK) {
                /* Coherent by construction: every row's payload encodes
                 * the exact accepted epoch. */
                MOQ_TEST_CHECK_EQ_U64(got, e);
                for (uint32_t i = 0; i < SNAP_LANES; i++) {
                    MOQ_TEST_CHECK_EQ_U64(rows[i].core.ingested_total,
                                          snap_encode(i, e));
                }
                rendered++;
                break;
            }
            MOQ_TEST_CHECK(rc == MOQR_ERR_WOULD_BLOCK);
            sched_yield();
        }
    }
    MOQ_TEST_CHECK_EQ_INT(rendered, SNAP_EPOCHS);
    atomic_store(&stop, 1);
    for (uint32_t i = 0; i < SNAP_LANES; i++) {
        MOQ_TEST_CHECK(pthread_join(tids[i], NULL) == 0);
        MOQ_TEST_CHECK_EQ_INT(lanes[i].errs, 0);
    }
    /* Post-join: the rows read directly, no further epoch requested. */
    uint64_t e_final = SNAP_EPOCHS, got = 0;
    MOQ_TEST_CHECK(moqr_cli_snapshot_collect(&snap, fixed_epoch, &e_final,
                                             rows, &got) == MOQR_OK);
    moqr_cli_snapshot_destroy(&snap);
    MOQ_TEST_PASS("snapshot_protocol_mt");
    return failures;
}

/* A failing allocator: succeeds until `fail_after` allocations have been
 * served, then refuses — the coordinator's allocation-failure pin. */
typedef struct failing_alloc {
    moq_alloc_t vt;
    int         served;
    int         fail_after;   /* allocations to allow before refusing */
} failing_alloc_t;

static void *
fa_alloc(size_t n, void *ctx)
{
    failing_alloc_t *a = ctx;
    if (a->served >= a->fail_after) {
        return NULL;
    }
    a->served++;
    return malloc(n);
}

static void *
fa_realloc(void *p, size_t o, size_t n, void *ctx)
{
    (void)o;
    (void)ctx;
    return realloc(p, n);
}

static void
fa_free(void *p, size_t n, void *ctx)
{
    (void)n;
    (void)ctx;
    free(p);
}

static void
fa_init(failing_alloc_t *a, int fail_after)
{
    memset(a, 0, sizeof(*a));
    a->vt.ctx = a;
    a->vt.alloc = fa_alloc;
    a->vt.realloc = fa_realloc;
    a->vt.free = fa_free;
    a->fail_after = fail_after;
}

/* A counting allocator that tracks live bytes: the independent oracle for the
 * admission delta — it measures the bytes a runtime actually requests, with no
 * knowledge of the capacity formula, so it catches a model term the formula
 * omits or mis-sizes. */
typedef struct count_alloc {
    moq_alloc_t vt;
    long        live;
} count_alloc_t;

static void *
ka_alloc(size_t n, void *ctx)
{
    count_alloc_t *a = ctx;
    void         *p = malloc(n);
    if (p != NULL) {
        a->live += (long)n;
    }
    return p;
}

static void *
ka_realloc(void *p, size_t o, size_t n, void *ctx)
{
    count_alloc_t *a = ctx;
    void          *q = realloc(p, n);
    if (q != NULL) {
        a->live += (long)n - (long)o;
    }
    return q;
}

static void
ka_free(void *p, size_t n, void *ctx)
{
    count_alloc_t *a = ctx;
    if (p != NULL) {
        a->live -= (long)n;
    }
    free(p);
}

static void
ka_init(count_alloc_t *a)
{
    memset(a, 0, sizeof(*a));
    a->vt.ctx = a;
    a->vt.alloc = ka_alloc;
    a->vt.realloc = ka_realloc;
    a->vt.free = ka_free;
}

static void
no_emit(void *ectx, const char *doc, size_t len, uint64_t epoch)
{
    (void)doc;
    (void)len;
    (void)epoch;
    (*(int *)ectx)++;
}

/* Coordinator allocation failures surface as NOMEM — a distinct, retryable
 * result the CLI diagnoses (never OK, never mistaken for an incomplete
 * epoch) and NOTHING is emitted. Both transient allocations are covered:
 * the row copy and the document buffer. Also the lane bound: the snapshot
 * refuses more lanes than the coordinator's fixed view assembly holds. */
static int
test_snapshot_coord_failures(void)
{
    int failures = 0;
    moqr_obs_labels_t labels[2] = {
        { 0, "sim", "draft-18" },
        { 1, "sim", "draft-18" },
    };
    for (int fail_at = 0; fail_at <= 2; fail_at++) {
        /* fail_at=0: the transient row copy fails; =1: the initial
         * document buffer fails; =2: the document REGROW fails (the
         * 2-shard exposition outgrows the first 32 KiB buffer). init's
         * own row-block allocation is budgeted separately (+1). */
        failing_alloc_t fa;
        fa_init(&fa, 1 + fail_at);
        moqr_cli_snapshot_t snap;
        MOQ_TEST_CHECK(moqr_cli_snapshot_init(&snap, 2, &fa.vt) == MOQR_OK);
        moqr_cli_snapshot_stats_t st;
        memset(&st, 0, sizeof(st));
        st.shard_stats_valid = true;
        moqr_cli_snapshot_publish(&snap, 0, &st, 1);
        moqr_cli_snapshot_publish(&snap, 1, &st, 1);
        uint64_t e = 1, got = 0;
        int emitted = 0;
        MOQ_TEST_CHECK(moqr_cli_coord_dump(&snap, labels, fixed_epoch, &e,
                                           no_emit, &emitted, &got) ==
                       MOQR_ERR_NOMEM);
        MOQ_TEST_CHECK_EQ_INT(emitted, 0);
        /* EVERY failure names the epoch it served — the caller's per-epoch
         * de-spam keys on it, so a zero here would mute the diagnostic. */
        MOQ_TEST_CHECK_EQ_U64(got, 1);
        MOQ_TEST_CHECK(moqr_cli_coord_dump(&snap, labels, NULL, NULL,
                                           no_emit, &emitted, &got) ==
                       MOQR_ERR_INVAL);
        /* Recovery: with allocations flowing again, the same epoch dumps. */
        fa.fail_after = 1000000;
        MOQ_TEST_CHECK(moqr_cli_coord_dump(&snap, labels, fixed_epoch, &e,
                                           no_emit, &emitted, &got) ==
                       MOQR_OK);
        MOQ_TEST_CHECK_EQ_INT(emitted, 1);
        MOQ_TEST_CHECK_EQ_U64(got, 1);
        moqr_cli_snapshot_destroy(&snap);
    }
    /* The lane bound: MOQR_SHARDS_MAX is accepted, one past it refused. */
    {
        const moq_alloc_t *alloc = moq_alloc_default();
        moqr_cli_snapshot_t snap;
        MOQ_TEST_CHECK(moqr_cli_snapshot_init(&snap, MOQR_SHARDS_MAX,
                                              alloc) == MOQR_OK);
        moqr_cli_snapshot_destroy(&snap);
        MOQ_TEST_CHECK(moqr_cli_snapshot_init(&snap, MOQR_SHARDS_MAX + 1u,
                                              alloc) == MOQR_ERR_INVAL);
    }
    MOQ_TEST_PASS("snapshot_coord_failures");
    return failures;
}

/* -- the PRODUCTION coordinator flow over a live runtime --------------------- */

/* Captured emissions from moqr_cli_coord_dump (the production emit seam). */
typedef struct emit_cap {
    int      calls;
    uint64_t last_epoch;
    int      bad_docs;
} emit_cap_t;

static void
capture_emit(void *ectx, const char *doc, size_t len, uint64_t epoch)
{
    emit_cap_t *c = ectx;
    c->calls++;
    c->last_epoch = epoch;
    if (len == 0 || strlen(doc) != len ||
        strstr(doc, "moqrelay_process_objects_ingested_total") == NULL ||
        strstr(doc, "moqrelay_journal_epoch{shard=\"0\"") == NULL ||
        strstr(doc, "moqrelay_channel_enqueues_total") == NULL) {
        c->bad_docs++;
    }
}

typedef struct coord_lane {
    moqr_shards_t       *s;
    moqr_cli_snapshot_t *snap;
    uint16_t             shard;
    _Atomic unsigned    *epoch;
    _Atomic int         *stop;
    int                  errs;
} coord_lane_t;

/* A lane: churns its shard's announce state (journals and mailboxes mutate
 * continuously on BOTH shards through the mirrors) while publishing its
 * metrics row for every newly requested epoch — the production lane shape. */
static void *
coord_lane_run(void *arg)
{
    coord_lane_t *t = arg;
    moqr_binding_t pub;
    if (moqr_core_binding_open(moqr_shards_core(t->s, t->shard), 1, &pub) !=
        MOQR_OK) {
        t->errs++;
        return NULL;
    }
    char nm[16];
    snprintf(nm, sizeof(nm), "cp%u", (unsigned)t->shard);
    moq_bytes_t part = { (const uint8_t *)nm, (uint32_t)strlen(nm) };
    moqr_ns_t ns = { &part, 1 };
    unsigned last = 0;
    bool announced = false;
    while (!atomic_load(t->stop)) {
        /* Journal churn: announce/withdraw flips candidates, mirrors, and
         * journal epochs on both shards every few steps. */
        moqr_result_t rc =
            announced
                ? moqr_core_unannounce(moqr_shards_core(t->s, t->shard), pub,
                                       ns)
                : moqr_core_announce(moqr_shards_core(t->s, t->shard), pub,
                                     ns);
        if (rc != MOQR_OK) {
            t->errs++;
        }
        announced = !announced;
        for (int r = 0; r < 2; r++) {
            uint64_t mask = 0;
            if (moqr_shards_step_shard(t->s, t->shard, 1000, &mask) !=
                MOQR_OK) {
                t->errs++;
            }
        }
        unsigned e = atomic_load(t->epoch);
        if (e != last) {
            moqr_cli_snapshot_stats_t st;
            memset(&st, 0, sizeof(st));
            moqr_core_get_stats(moqr_shards_core(t->s, t->shard), &st.core);
            moqr_bind_get_stats(moqr_shards_bind(t->s, t->shard), &st.bind);
            st.shard_stats_valid =
                moqr_shards_get_stats(t->s, t->shard, &st.shard) == MOQR_OK;
            moqr_cli_snapshot_publish(t->snap, t->shard, &st, e);
            last = e;
        }
        sched_yield();
    }
    return NULL;
}

/* The PRODUCTION coordinator (moqr_cli_coord_dump — the exact seam
 * cmd_serve_lanes drives) against a LIVE runtime: lanes churn journals and
 * mailboxes on their own threads while the coordinator renders complete
 * epochs from the published rows alone. The seam is never handed the shard
 * runtime, so route/journal traversal is impossible by construction; under
 * TSan this test additionally proves the whole flow touches nothing a live
 * lane mutates. */
static int
test_snapshot_coord_production(void)
{
    int failures = 0;
    enum { COORD_EPOCHS = 12 };
    const moq_alloc_t *alloc = moq_alloc_default();
    moqr_shards_cfg_t scfg;
    moqr_shards_cfg_init_sized(&scfg, sizeof(scfg), alloc);
    scfg.shards = 2;
    scfg.live_visibility = true;
    moqr_shards_t *s = NULL;
    MOQ_TEST_CHECK(moqr_shards_create(&scfg, &s) == MOQR_OK);
    moqr_cli_snapshot_t snap;
    MOQ_TEST_CHECK(moqr_cli_snapshot_init(&snap, 2, alloc) == MOQR_OK);
    moqr_obs_labels_t labels[2] = {
        { 0, "sim", "draft-18" },
        { 1, "sim", "draft-18" },
    };

    _Atomic unsigned epoch = 0;
    _Atomic int stop = 0;
    coord_lane_t lanes[2] = {
        { s, &snap, 0, &epoch, &stop, 0 },
        { s, &snap, 1, &epoch, &stop, 0 },
    };
    pthread_t tids[2];
    MOQ_TEST_CHECK(pthread_create(&tids[0], NULL, coord_lane_run,
                                  &lanes[0]) == 0);
    MOQ_TEST_CHECK(pthread_create(&tids[1], NULL, coord_lane_run,
                                  &lanes[1]) == 0);

    emit_cap_t cap = { 0, 0, 0 };
    int rendered = 0;
    for (unsigned e = 1; e <= COORD_EPOCHS; e++) {
        atomic_store(&epoch, e);
        for (int spin = 0; spin < 2000000; spin++) {
            uint64_t got = 0;
            moqr_result_t rc =
                moqr_cli_coord_dump(&snap, labels, atomic_epoch_read, &epoch,
                                    capture_emit, &cap, &got);
            if (rc == MOQR_OK) {
                MOQ_TEST_CHECK_EQ_U64(got, e);
                MOQ_TEST_CHECK_EQ_U64(cap.last_epoch, e);
                rendered++;
                break;
            }
            MOQ_TEST_CHECK(rc == MOQR_ERR_WOULD_BLOCK);
            sched_yield();
        }
    }
    MOQ_TEST_CHECK_EQ_INT(rendered, COORD_EPOCHS);
    MOQ_TEST_CHECK_EQ_INT(cap.calls, COORD_EPOCHS);
    MOQ_TEST_CHECK_EQ_INT(cap.bad_docs, 0);
    atomic_store(&stop, 1);
    MOQ_TEST_CHECK(pthread_join(tids[0], NULL) == 0);
    MOQ_TEST_CHECK(pthread_join(tids[1], NULL) == 0);
    MOQ_TEST_CHECK_EQ_INT(lanes[0].errs, 0);
    MOQ_TEST_CHECK_EQ_INT(lanes[1].errs, 0);
    moqr_cli_snapshot_destroy(&snap);
    moqr_shards_destroy(s);
    MOQ_TEST_PASS("snapshot_coord_production");
    return failures;
}

int
main(void)
{
    int failures = 0;
    moqr_cli_config_t cfg;
    char err[128];

    /* Minimal valid config: port only; defaults fill the rest. The msquic
     * listener is exact-version, so the default is ONE offer (the newest). */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":4443}}", &cfg, err,
                         sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_INT(cfg.port, 4443);
    MOQ_TEST_CHECK(strcmp(cfg.host, "0.0.0.0") == 0);
    MOQ_TEST_CHECK_EQ_SIZE(cfg.alpn_count, (size_t)1);
    MOQ_TEST_CHECK(strcmp(cfg.alpns[0], "moqt-18") == 0);
    MOQ_TEST_CHECK_EQ_U64(cfg.version, 18);
    MOQ_TEST_CHECK_EQ_U64(cfg.lanes, 1);   /* default single-lane */
    MOQ_TEST_CHECK(!cfg.insecure_skip_verify);

    /* listener.lanes: accepted at the boundaries; rejected outside 1..64 and
     * for a non-number, each with an error naming the key. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":2}}", &cfg, err,
                         sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(cfg.lanes, 2);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":64}}", &cfg, err,
                         sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(cfg.lanes, 64);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":0}}", &cfg, err,
                         sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "lanes") != NULL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":65}}", &cfg, err,
                         sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "lanes") != NULL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":\"two\"}}", &cfg,
                         err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "lanes") != NULL);

    /* Full config round-trips into core budgets. */
    const char *full =
        "{\"listener\":{\"transport\":\"msquic\",\"host\":\"::1\","
        "\"port\":9,\"cert\":\"c.pem\",\"key\":\"k.pem\","
        "\"versions\":[16],\"insecure_skip_verify\":true},"
        "\"budgets\":{\"max_tracks\":7,\"max_subs\":11,"
        "\"log\":{\"max_groups\":3,\"max_bytes\":4096,\"max_age_us\":50}},"
        "\"linger_us\":250}";
    MOQ_TEST_CHECK(parse(full, &cfg, err, sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK(strcmp(cfg.host, "::1") == 0);
    MOQ_TEST_CHECK(strcmp(cfg.cert, "c.pem") == 0);
    MOQ_TEST_CHECK_EQ_SIZE(cfg.alpn_count, (size_t)1);
    MOQ_TEST_CHECK(strcmp(cfg.alpns[0], "moqt-16") == 0);
    MOQ_TEST_CHECK_EQ_U64(cfg.version, 16);
    MOQ_TEST_CHECK(cfg.insecure_skip_verify);
    MOQ_TEST_CHECK_EQ_U64(cfg.core.max_tracks, 7);
    MOQ_TEST_CHECK_EQ_U64(cfg.core.max_subs, 11);
    MOQ_TEST_CHECK_EQ_U64(cfg.core.log_budget.max_groups, 3);
    MOQ_TEST_CHECK_EQ_U64(cfg.core.log_budget.max_bytes, 4096);
    MOQ_TEST_CHECK_EQ_U64(cfg.core.linger_us, 250);

    /* Missing required listener fields. */
    MOQ_TEST_CHECK(parse("{}", &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "listener") != NULL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"host\":\"x\"}}", &cfg, err,
                         sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "port") != NULL);

    /* Bad transport (picoquic is no longer a production transport). */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"transport\":\"tcp\"}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "transport") != NULL);
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1,\"transport\":\"picoquic\"}}", &cfg,
              err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "transport") != NULL);

    /* listener.versions is an ORDERED, non-empty set. Everything malformed
     * fails closed; a multi-entry list is accepted and keeps its order. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[17]}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[]}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[18,18]}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "duplicate") != NULL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[true]}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[18.5]}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[\"18\"]}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":18}}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1,\"versions\":[18,16,18,16,18]}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);

    /* Default is exactly [18], and one entry keeps exact-listener semantics:
     * the label is the bare ALPN and the plan carries no list. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1}}", &cfg, err,
                         sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK(cfg.version_count == 1);
    MOQ_TEST_CHECK(cfg.versions[0] == MOQ_VERSION_DRAFT_18);
    MOQ_TEST_CHECK(strcmp(cfg.alpn_set, "moqt-18") == 0);
    {
        moqr_cli_version_plan_t pl;
        moqr_cli_version_plan(&cfg, &pl);
        MOQ_TEST_CHECK(pl.exact == MOQ_VERSION_DRAFT_18);
        MOQ_TEST_CHECK(pl.list == NULL);
        MOQ_TEST_CHECK(pl.count == 0);   /* count 0 IS the exact plan */
        /* An exact plan is never ambiguous: after a successful parse it cannot
         * have both a zero version and a zero count, which would read as "no
         * plan at all" to the caller. */
        MOQ_TEST_CHECK(!(pl.exact == 0 && pl.count == 0));
        MOQ_TEST_CHECK(pl.count != 1);   /* count 1 is never produced */
    }
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[16]}}",
                         &cfg, err, sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK(cfg.version_count == 1);
    MOQ_TEST_CHECK(strcmp(cfg.alpn_set, "moqt-16") == 0);
    {
        moqr_cli_version_plan_t pl;
        moqr_cli_version_plan(&cfg, &pl);
        MOQ_TEST_CHECK(pl.exact == MOQ_VERSION_DRAFT_16);
        MOQ_TEST_CHECK(pl.count == 0);
        MOQ_TEST_CHECK(!(pl.exact == 0 && pl.count == 0));
        MOQ_TEST_CHECK(pl.count != 1);
    }

    /* A multi-entry list is accepted, ORDER PRESERVED, and its plan hands the
     * ordered list over instead of an exact version. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[18,16]}}",
                         &cfg, err, sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK(cfg.version_count == 2);
    if (cfg.version_count == 2) {
        MOQ_TEST_CHECK(cfg.versions[0] == MOQ_VERSION_DRAFT_18);
        MOQ_TEST_CHECK(cfg.versions[1] == MOQ_VERSION_DRAFT_16);
    }
    MOQ_TEST_CHECK(strcmp(cfg.alpn_set, "moqt-18+moqt-16") == 0);
    {
        moqr_cli_version_plan_t pl;
        moqr_cli_version_plan(&cfg, &pl);
        MOQ_TEST_CHECK(pl.exact == 0);
        MOQ_TEST_CHECK(pl.count == 2);
        MOQ_TEST_CHECK(pl.list != NULL);
        if (pl.list != NULL && pl.count == 2) {
            MOQ_TEST_CHECK(pl.list[0] == MOQ_VERSION_DRAFT_18);
            MOQ_TEST_CHECK(pl.list[1] == MOQ_VERSION_DRAFT_16);
        }
    }
    /* The reverse order is a DIFFERENT preference, not a normalized set. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"versions\":[16,18]}}",
                         &cfg, err, sizeof(err)) == MOQR_OK);
    if (cfg.version_count == 2) {
        MOQ_TEST_CHECK(cfg.versions[0] == MOQ_VERSION_DRAFT_16);
        MOQ_TEST_CHECK(cfg.versions[1] == MOQ_VERSION_DRAFT_18);
    }
    MOQ_TEST_CHECK(strcmp(cfg.alpn_set, "moqt-16+moqt-18") == 0);

    /* Bad port. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":70000}}", &cfg, err,
                         sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":0}}", &cfg, err,
                         sizeof(err)) == MOQR_ERR_INVAL);

    /* Oversized budgets are config errors, not requests. */
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},"
              "\"budgets\":{\"max_tracks\":9999999}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"budgets\":{\"log\":"
              "{\"max_bytes\":2199023255552}}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);

    /* Unknown keys fail loudly. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1},\"typo\":1}", &cfg,
                         err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"prot\":1}}", &cfg,
                         err, sizeof(err)) == MOQR_ERR_INVAL);

    /* Not JSON at all. */
    MOQ_TEST_CHECK(parse("not json", &cfg, err, sizeof(err)) ==
                   MOQR_ERR_INVAL);

    /* Negative and overflowing numbers are rejected, not wrapped. A bare
     * "-1" must not become UINT64_MAX in an uncapped field (linger_us). */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1},\"linger_us\":-1}",
                         &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"budgets\":{\"log\":"
              "{\"max_age_us\":-1}}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":-1}}", &cfg, err,
                         sizeof(err)) == MOQR_ERR_INVAL);
    /* Overflow past 2^64 (ERANGE). */
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"linger_us\":"
              "99999999999999999999999}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);

    /* Telemetry object: trace ring depth parses into the config. */
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},"
              "\"telemetry\":{\"trace_ring_records\":4096}}",
              &cfg, err, sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(cfg.telemetry.trace_ring_records, 4096);
    /* Absent telemetry leaves the default (0 = library default). */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1}}", &cfg, err,
                         sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_U64(cfg.telemetry.trace_ring_records, 0);
    /* Unknown telemetry key fails loudly. */
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"telemetry\":{\"depth\":8}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "telemetry") != NULL);
    /* Oversized and non-object telemetry are config errors. */
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"telemetry\":"
              "{\"trace_ring_records\":9999999}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"telemetry\":5}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    /* Negative trace depth is rejected, not wrapped. */
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"telemetry\":"
              "{\"trace_ring_records\":-1}}",
              &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);

    /* Capacity-only path: a parsed config yields a nonzero ceiling. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":4443}}", &cfg, err,
                         sizeof(err)) == MOQR_OK);
    cfg.core.alloc = moq_alloc_default();
    moqr_core_capacity_t cap;
    moqr_core_capacity_describe(&cfg.core, &cap);
    MOQ_TEST_CHECK(cap.total_bytes > 0);
    MOQ_TEST_CHECK_EQ_U64(cap.total_bytes,
                          cap.structure_bytes + cap.payload_bytes);

    /* The telemetry knob is real, not just parsed: build_core attaches a
     * trace ring that records actual core transitions, and the configured
     * depth takes effect (a 4-record ring caps the retained tail while the
     * total climbs). */
    {
        moqr_cli_config_t bc;
        MOQ_TEST_CHECK(
            parse("{\"listener\":{\"port\":1},"
                  "\"budgets\":{\"max_bindings\":32},"
                  "\"telemetry\":{\"trace_ring_records\":4}}",
                  &bc, err, sizeof(err)) == MOQR_OK);
        const moq_alloc_t *alloc = moq_alloc_default();
        moqr_trace_t *trace = NULL;
        moqr_core_t *core = NULL;
        MOQ_TEST_CHECK(moqr_cli_build_core(&bc, alloc, &trace, &core) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(trace != NULL && core != NULL);
        MOQ_TEST_CHECK_EQ_U64(moqr_trace_count(trace), 0);   /* nothing yet */

        /* Configured budgets.max_bindings resolves into the core limit that
         * serve() feeds to the transport admission cap (tcfg.max_connections =
         * moqr_core_get_limits(core).max_bindings) — so a non-default budget
         * moves the transport cap, not just the core pool. */
        moqr_core_limits_t lim;
        moqr_core_get_limits(core, &lim);
        MOQ_TEST_CHECK_EQ_U64(lim.max_bindings, 32);

        /* A real transition (binding open → ROUTE_ADD) lands on the ring. */
        moqr_binding_t b0;
        MOQ_TEST_CHECK(moqr_core_binding_open(core, 1, &b0) == MOQR_OK);
        MOQ_TEST_CHECK(moqr_trace_count(trace) >= 1);

        /* Overflow the 4-record ring; retained caps at 4 though more emit —
         * proving the configured depth flowed into the ring, not a default. */
        for (uint64_t i = 0; i < 10; i++) {
            moqr_binding_t bx;
            MOQ_TEST_CHECK(moqr_core_binding_open(core, 100 + i, &bx) ==
                           MOQR_OK);
        }
        moqr_trace_rec_t recs[16];
        size_t retained = moqr_trace_read(trace, recs, 16);
        MOQ_TEST_CHECK_EQ_SIZE(retained, (size_t)4);
        MOQ_TEST_CHECK(moqr_trace_count(trace) > 4);

        moqr_core_destroy(core);
        moqr_trace_destroy(trace);
    }

    /* The trace ring is accounted in the process ceiling: a deeper ring
     * raises the reported total by exactly its record-array growth, while the
     * (identical) core budgets stay fixed. Proves capacity output tracks
     * what build_core actually reserves, not just the core pools. */
    {
        moqr_cli_config_t ca1, ca2, ca0;
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1},"
                             "\"telemetry\":{\"trace_ring_records\":1024}}",
                             &ca1, err, sizeof(err)) == MOQR_OK);
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1},"
                             "\"telemetry\":{\"trace_ring_records\":8192}}",
                             &ca2, err, sizeof(err)) == MOQR_OK);
        const moq_alloc_t *alloc = moq_alloc_default();
        moqr_cli_capacity_t p1, p2;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&ca1, alloc, 0, &p1) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&ca2, alloc, 0, &p2) ==
                       MOQR_OK);
        /* Identical core budgets => identical core AND bind terms. */
        MOQ_TEST_CHECK_EQ_U64(p1.core_structure_bytes,
                              p2.core_structure_bytes);
        MOQ_TEST_CHECK_EQ_U64(p1.core_payload_bytes, p2.core_payload_bytes);
        MOQ_TEST_CHECK(p1.bind_structure_bytes > 0);
        MOQ_TEST_CHECK_EQ_U64(p1.bind_structure_bytes,
                              p2.bind_structure_bytes);
        /* lanes=1 is the direct composition: core + bind + trace, no shard
         * container, no CLI runtime bytes. */
        MOQ_TEST_CHECK_EQ_U64(p1.cross_shard_bytes, 0);
        MOQ_TEST_CHECK_EQ_U64(p1.cli_runtime_bytes, 0);
        MOQ_TEST_CHECK_EQ_U64(p1.total_bytes, p1.core_structure_bytes +
                                                  p1.core_payload_bytes +
                                                  p1.bind_structure_bytes +
                                                  p1.trace_bytes);
        MOQ_TEST_CHECK(p2.trace_bytes > p1.trace_bytes);
        MOQ_TEST_CHECK_EQ_U64(p2.trace_bytes - p1.trace_bytes,
                              (uint64_t)(8192 - 1024) *
                                  sizeof(moqr_trace_rec_t));
        MOQ_TEST_CHECK_EQ_U64(p2.total_bytes - p1.total_bytes,
                              p2.trace_bytes - p1.trace_bytes);
        /* Default depth (0) still accounts a non-zero ring. */
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1}}", &ca0, err,
                             sizeof(err)) == MOQR_OK);
        moqr_cli_capacity_t p0;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&ca0, alloc, 0, &p0) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(p0.trace_bytes > 0);

        /* lanes>1 describes the WHOLE process off the same shared builder
         * serve consumes: cross-shard terms appear, the CLI runtime joins
         * the total, and the per-lane clamp shows in usable bindings. */
        moqr_cli_config_t cl1, cl4;
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":1}}", &cl1,
                             err, sizeof(err)) == MOQR_OK);
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":4}}", &cl4,
                             err, sizeof(err)) == MOQR_OK);
        moqr_cli_capacity_t q1, q4;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&cl1, alloc, 0, &q1) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&cl4, alloc, 512, &q4) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(q4.total_bytes > q1.total_bytes);
        MOQ_TEST_CHECK(q4.cross_shard_bytes > 0);
        MOQ_TEST_CHECK_EQ_U64(q4.cli_runtime_bytes,
                              512 + moqr_cli_snapshot_bytes(4));
        /* Default core max_bindings 64: 4 lanes leave 60 external each. */
        MOQ_TEST_CHECK_EQ_U64(q4.usable_bindings_per_shard, 60);
        MOQ_TEST_CHECK_EQ_U64(q1.usable_bindings_per_shard, 64);
        /* The same builder underpins both commands: its resolved config is
         * byte-stable across two invocations. */
        moqr_shards_cfg_t b1, b2;
        moqr_cli_build_shards_cfg(&cl4, alloc, &b1);
        moqr_cli_build_shards_cfg(&cl4, alloc, &b2);
        MOQ_TEST_CHECK(memcmp(&b1, &b2, sizeof(b1)) == 0);
        /* Production admission is ON whenever the CLI runs multiple lanes
         * (the one place the rule lives), OFF at a single lane. */
        MOQ_TEST_CHECK(b1.admit_remote_demand);   /* lanes=4 -> on */
        MOQ_TEST_CHECK_EQ_U64(b1.bind_cfg.max_conns, 60);
        {
            moqr_shards_cfg_t b1a;
            moqr_cli_build_shards_cfg(&cl1, alloc, &b1a);
            MOQ_TEST_CHECK(!b1a.admit_remote_demand);   /* lanes=1 -> off */
        }
        /* CLI-composition overflow refuses INVAL: a poisoned serve-context
         * size must never wrap into a small OK total. */
        moqr_cli_capacity_t qo;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&cl4, alloc, SIZE_MAX,
                                                  &qo) == MOQR_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_U64(qo.total_bytes, 0);
    }

    /* ---- production admission flip: the builder rule + exact capacity
     * delta, proven structurally (not through slack) ---- */
    {
        const moq_alloc_t *alloc = moq_alloc_default();
        /* lanes=1 off; lanes 2, 4, and the max lane count all on — the
         * builder is the sole authority, and the resolved shards limits +
         * capacity admission flag both track it. */
        struct {
            uint32_t lanes;
            bool     admit;
        } cases[] = { { 1, false }, { 2, true }, { 4, true },
                      { MOQR_CLI_MAX_LANES, true } };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            /* The maximum lane count needs a binding pool large enough to
             * leave one external slot per lane after each manager consumes
             * its K slots (the K>1 binding-budget guard); every case below
             * uses a headroom pool so all four RESOLVE. */
            char js[128];
            snprintf(js, sizeof(js),
                     "{\"listener\":{\"port\":1,\"lanes\":%u},"
                     "\"budgets\":{\"max_bindings\":256}}",
                     cases[i].lanes);
            moqr_cli_config_t c;
            MOQ_TEST_CHECK(parse(js, &c, err, sizeof(err)) == MOQR_OK);
            moqr_shards_cfg_t sc;
            moqr_cli_build_shards_cfg(&c, alloc, &sc);
            MOQ_TEST_CHECK(sc.admit_remote_demand == cases[i].admit);
            /* The resolved limits and the capacity model agree with the
             * builder — no third place decides admission. */
            moqr_shards_limits_t lim;
            MOQ_TEST_CHECK(moqr_shards_cfg_resolve(&sc, &lim) == MOQR_OK);
            MOQ_TEST_CHECK(lim.admit == cases[i].admit);
            moqr_shards_capacity_t cap;
            MOQ_TEST_CHECK(moqr_shards_capacity_describe(&sc, &cap) ==
                           MOQR_OK);
            MOQ_TEST_CHECK(cap.admission == cases[i].admit);
        }

        /* The EXACT admission delta, structurally: clone one K>1 builder
         * output and turn admission OFF only in the comparison copy. The
         * on/off shard structures must differ by ONLY the admission
         * progress-table term the capacity model adds; every other term is
         * byte-equal. Since the private progress-slot size is not a public
         * constant, the delta is taken from the model's OWN computed term
         * (shards_structure on minus off) and proven structural three ways:
         * it is confined to the shard structure, it scales EXACTLY linearly
         * with K (so it is the per-shard progress storage times K, not a
         * fixed overhead), and it scales linearly with the subgroup-slot
         * budget (so it is the pend x sg progress table, not some other
         * admission cost). */
        moqr_cli_config_t c4;
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":4}}", &c4,
                             err, sizeof(err)) == MOQR_OK);
        moqr_shards_cfg_t on, off;
        moqr_cli_build_shards_cfg(&c4, alloc, &on);
        off = on;
        off.admit_remote_demand = false;
        MOQ_TEST_CHECK(on.admit_remote_demand);
        moqr_shards_capacity_t cap_on, cap_off;
        MOQ_TEST_CHECK(moqr_shards_capacity_describe(&on, &cap_on) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(moqr_shards_capacity_describe(&off, &cap_off) ==
                       MOQR_OK);
        uint64_t delta = cap_on.shards_structure_bytes -
                         cap_off.shards_structure_bytes;
        MOQ_TEST_CHECK(delta > 0);
        /* Only the shard structure moves: every other term is byte-equal. */
        MOQ_TEST_CHECK_EQ_U64(cap_on.channel_byte_ceiling,
                              cap_off.channel_byte_ceiling);
        MOQ_TEST_CHECK_EQ_U64(cap_on.canon_byte_ceiling,
                              cap_off.canon_byte_ceiling);
        MOQ_TEST_CHECK_EQ_U64(cap_on.staging_byte_ceiling,
                              cap_off.staging_byte_ceiling);
        MOQ_TEST_CHECK_EQ_U64(cap_on.core_structure_bytes,
                              cap_off.core_structure_bytes);
        MOQ_TEST_CHECK_EQ_U64(cap_on.core_payload_bytes,
                              cap_off.core_payload_bytes);
        MOQ_TEST_CHECK_EQ_U64(cap_on.bind_structure_bytes,
                              cap_off.bind_structure_bytes);
        MOQ_TEST_CHECK_EQ_U64(cap_on.trace_bytes, cap_off.trace_bytes);
        /* The ceiling moves by exactly the admission delta. */
        MOQ_TEST_CHECK_EQ_U64(cap_on.relay_alloc_ceiling -
                                  cap_off.relay_alloc_ceiling,
                              delta);
        /* The CLI total moves by exactly the same delta (cli_runtime is
         * admission-independent). */
        moqr_cli_capacity_t con;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&c4, alloc,
                                                  sizeof(void *) * 8, &con) ==
                       MOQR_OK);
        MOQ_TEST_CHECK_EQ_U64(con.total_bytes,
                              cap_on.relay_alloc_ceiling +
                                  con.cli_runtime_bytes);
        MOQ_TEST_CHECK_EQ_U64(con.total_bytes -
                                  (cap_off.relay_alloc_ceiling +
                                   con.cli_runtime_bytes),
                              delta);
        /* K-linearity: the same delta at lanes=2 is EXACTLY half (the
         * per-shard progress storage times K). */
        {
            moqr_cli_config_t c2;
            MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":2}}",
                                 &c2, err, sizeof(err)) == MOQR_OK);
            moqr_shards_cfg_t on2 = on, off2;
            moqr_cli_build_shards_cfg(&c2, alloc, &on2);
            off2 = on2;
            off2.admit_remote_demand = false;
            moqr_shards_capacity_t con2, coff2;
            MOQ_TEST_CHECK(moqr_shards_capacity_describe(&on2, &con2) ==
                           MOQR_OK);
            MOQ_TEST_CHECK(moqr_shards_capacity_describe(&off2, &coff2) ==
                           MOQR_OK);
            uint64_t d2 = con2.shards_structure_bytes -
                          coff2.shards_structure_bytes;
            /* delta(K=4)/4 == delta(K=2)/2 (constant per shard). */
            MOQ_TEST_CHECK_EQ_U64(delta * 2u, d2 * 4u);
        }
        /* Subgroup-slot linearity, EXACT: the admission term is
         * K·(2·pend·sg·psg + pend). Doubling subgroup_slots doubles the
         * 2·pend·sg part while the flat +pend part is unchanged, so
         *   sdelta == 2·delta − K·pend
         * must hold to the byte. This pins the term SHAPE (an affine function
         * of sg with the right slope-doubling and intercept), independent of
         * the private progress-slot size. */
        {
            moqr_shards_limits_t blim;
            MOQ_TEST_CHECK(moqr_shards_cfg_resolve(&on, &blim) == MOQR_OK);
            moqr_cli_config_t cs;
            MOQ_TEST_CHECK(parse(
                "{\"listener\":{\"port\":1,\"lanes\":4},\"budgets\":{"
                "\"cross_shard\":{\"subgroup_slots\":128}}}",
                &cs, err, sizeof(err)) == MOQR_OK);
            moqr_shards_cfg_t son, soff;
            moqr_cli_build_shards_cfg(&cs, alloc, &son);
            soff = son;
            soff.admit_remote_demand = false;
            moqr_shards_limits_t slim;
            MOQ_TEST_CHECK(moqr_shards_cfg_resolve(&son, &slim) == MOQR_OK);
            /* Premises: subgroup slots doubled; pend and K held constant. */
            MOQ_TEST_CHECK_EQ_U64(slim.sg_slots, 2u * (uint64_t)blim.sg_slots);
            MOQ_TEST_CHECK_EQ_U64(slim.pend_cap, blim.pend_cap);
            MOQ_TEST_CHECK_EQ_U64(slim.shards, blim.shards);
            moqr_shards_capacity_t scon, scoff;
            MOQ_TEST_CHECK(moqr_shards_capacity_describe(&son, &scon) ==
                           MOQR_OK);
            MOQ_TEST_CHECK(moqr_shards_capacity_describe(&soff, &scoff) ==
                           MOQR_OK);
            uint64_t sdelta = scon.shards_structure_bytes -
                              scoff.shards_structure_bytes;
            MOQ_TEST_CHECK_EQ_U64(sdelta,
                                  2u * delta -
                                      (uint64_t)slim.shards * slim.pend_cap);
        }

        /* Independent, NON-CIRCULAR pin: a live K>1 runtime requests exactly
         * the model's admission term more when admission is on. The counting
         * allocator knows nothing of the capacity formula — it measures the
         * bytes create() actually asks for — so an admission component the
         * model omits or mis-sizes makes the measured and described deltas
         * disagree. (The K-linearity and slot-shape checks above prove the
         * term's structure; this proves its magnitude against reality.) */
        {
            count_alloc_t on_a, off_a;
            ka_init(&on_a);
            ka_init(&off_a);
            moqr_shards_cfg_t rt_on, rt_off;
            moqr_cli_build_shards_cfg(&c4, &on_a.vt, &rt_on);
            moqr_cli_build_shards_cfg(&c4, &off_a.vt, &rt_off);
            rt_off.admit_remote_demand = false;
            MOQ_TEST_CHECK(rt_on.admit_remote_demand);
            moqr_shards_capacity_t mon, moff;
            MOQ_TEST_CHECK(moqr_shards_capacity_describe(&rt_on, &mon) ==
                           MOQR_OK);
            MOQ_TEST_CHECK(moqr_shards_capacity_describe(&rt_off, &moff) ==
                           MOQR_OK);
            uint64_t model_delta =
                mon.shards_structure_bytes - moff.shards_structure_bytes;
            /* same K=4 config as `on`/`off` above -> same described term. */
            MOQ_TEST_CHECK_EQ_U64(model_delta, delta);
            moqr_shards_t *rt_on_s = NULL, *rt_off_s = NULL;
            MOQ_TEST_CHECK(moqr_shards_create(&rt_on, &rt_on_s) == MOQR_OK);
            MOQ_TEST_CHECK(moqr_shards_create(&rt_off, &rt_off_s) == MOQR_OK);
            MOQ_TEST_CHECK(on_a.live > off_a.live);
            MOQ_TEST_CHECK_EQ_U64((uint64_t)(on_a.live - off_a.live),
                                  model_delta);
            moqr_shards_destroy(rt_on_s);
            moqr_shards_destroy(rt_off_s);
            /* clean teardown: the difference was solely the admission tables. */
            MOQ_TEST_CHECK_EQ_INT((int)on_a.live, 0);
            MOQ_TEST_CHECK_EQ_INT((int)off_a.live, 0);
        }

        /* K=1 on/off capacity is byte-identical: admission adds zero at a
         * single lane (no manager, no progress tables). */
        moqr_cli_config_t c1;
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":1}}", &c1,
                             err, sizeof(err)) == MOQR_OK);
        moqr_shards_cfg_t k1;
        moqr_cli_build_shards_cfg(&c1, alloc, &k1);
        MOQ_TEST_CHECK(!k1.admit_remote_demand);
        moqr_shards_cfg_t k1on = k1;
        k1on.admit_remote_demand = true;   /* force on: inert at K=1 */
        moqr_shards_capacity_t k1c_off, k1c_on;
        MOQ_TEST_CHECK(moqr_shards_capacity_describe(&k1, &k1c_off) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(moqr_shards_capacity_describe(&k1on, &k1c_on) ==
                       MOQR_OK);
        /* The resolver normalizes effective admission to (K>1 && configured),
         * so a forced admit is reported as OFF at a single lane: the two
         * descriptors — every field, including the admission flag — are
         * byte-identical (exact, no ceiling-only fallback). */
        MOQ_TEST_CHECK(!k1c_on.admission);
        MOQ_TEST_CHECK(memcmp(&k1c_off, &k1c_on, sizeof(k1c_off)) == 0);

        /* The serve composition seam (what cmd_serve_lanes consumes) is the
         * SAME builder output — admission preserved, never overwritten —
         * plus the facade cap = lanes * usable_bindings_per_shard (the
         * strict per-lane clamp, not lanes * max_bindings). */
        moqr_shards_cfg_t serve_scfg;
        uint32_t serve_max = 0;
        MOQ_TEST_CHECK(moqr_cli_serve_compose(&c4, alloc, &serve_scfg,
                                              &serve_max) == MOQR_OK);
        moqr_shards_cfg_t direct;
        moqr_cli_build_shards_cfg(&c4, alloc, &direct);
        MOQ_TEST_CHECK(memcmp(&serve_scfg, &direct, sizeof(direct)) == 0);
        MOQ_TEST_CHECK(serve_scfg.admit_remote_demand);   /* not overwritten */
        moqr_shards_limits_t sl;
        MOQ_TEST_CHECK(moqr_shards_cfg_resolve(&serve_scfg, &sl) == MOQR_OK);
        MOQ_TEST_CHECK_EQ_U64(serve_max, 4u * sl.usable_bindings);
        MOQ_TEST_CHECK(serve_max != 4u * 64u);   /* NOT the old coarse rule */
        /* lanes=1 serve composition: admission off, facade = usable (64). */
        moqr_shards_cfg_t s1cfg;
        uint32_t s1max = 0;
        MOQ_TEST_CHECK(moqr_cli_serve_compose(&c1, alloc, &s1cfg, &s1max) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(!s1cfg.admit_remote_demand);
        MOQ_TEST_CHECK_EQ_U64(s1max, 64u);
    }

    /* ---- budgets.cross_shard: strict operator keys for the K>1 pools ---- */
    {
        const moq_alloc_t *alloc = moq_alloc_default();
        /* Absent vs {}: byte-identical builder resolution and capacity. */
        moqr_cli_config_t ab, emp;
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":4}}", &ab,
                             err, sizeof(err)) == MOQR_OK);
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":4},"
                             "\"budgets\":{\"cross_shard\":{}}}",
                             &emp, err, sizeof(err)) == MOQR_OK);
        moqr_shards_cfg_t ba, be;
        moqr_cli_build_shards_cfg(&ab, alloc, &ba);
        moqr_cli_build_shards_cfg(&emp, alloc, &be);
        MOQ_TEST_CHECK(memcmp(&ba, &be, sizeof(ba)) == 0);
        moqr_cli_capacity_t ca_, ce_;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&ab, alloc, 0, &ca_) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&emp, alloc, 0, &ce_) ==
                       MOQR_OK);
        MOQ_TEST_CHECK_EQ_U64(ca_.total_bytes, ce_.total_bytes);

        /* Every key parses, maps through the ONE builder, and lands on the
         * shard config with distinct values. */
        moqr_cli_config_t full;
        MOQ_TEST_CHECK(parse(
            "{\"listener\":{\"port\":1,\"lanes\":4},\"budgets\":{"
            "\"cross_shard\":{\"journal_entries\":11,"
            "\"mailbox_entries\":12,\"demand_channel_entries\":13,"
            "\"demand_channel_bytes\":16777216,\"pending_demands\":14,"
            "\"subgroup_slots\":15}}}",
            &full, err, sizeof(err)) == MOQR_OK);
        moqr_shards_cfg_t bf;
        moqr_cli_build_shards_cfg(&full, alloc, &bf);
        MOQ_TEST_CHECK_EQ_U64(bf.journal_entries, 11);
        MOQ_TEST_CHECK_EQ_U64(bf.mailbox_entries, 12);
        MOQ_TEST_CHECK_EQ_U64(bf.demand_channel_entries, 13);
        MOQ_TEST_CHECK_EQ_U64(bf.demand_channel_bytes, 16777216);
        MOQ_TEST_CHECK_EQ_U64(bf.pending_demand_entries, 14);
        MOQ_TEST_CHECK_EQ_U64(bf.pump_subgroup_slots, 15);
        /* ...and each knob reaches the capacity model: bumping the mailbox
         * moves the canon ceiling; the byte cap moves the channel term. */
        moqr_shards_capacity_t sf, sb;
        MOQ_TEST_CHECK(moqr_shards_capacity_describe(&bf, &sf) == MOQR_OK);
        MOQ_TEST_CHECK(moqr_shards_capacity_describe(&ba, &sb) == MOQR_OK);
        MOQ_TEST_CHECK(sf.canon_byte_ceiling != sb.canon_byte_ceiling);
        MOQ_TEST_CHECK(sf.channel_byte_ceiling != sb.channel_byte_ceiling);
        /* Boundary acceptance: 0 (library default) and the entry maximum. */
        moqr_cli_config_t bd;
        MOQ_TEST_CHECK(parse(
            "{\"listener\":{\"port\":1,\"lanes\":2},\"budgets\":{"
            "\"cross_shard\":{\"journal_entries\":0,"
            "\"mailbox_entries\":1048576}}}",
            &bd, err, sizeof(err)) == MOQR_OK);
        moqr_shards_cfg_t bbd;
        moqr_cli_build_shards_cfg(&bd, alloc, &bbd);
        MOQ_TEST_CHECK_EQ_U64(bbd.journal_entries, 0);
        MOQ_TEST_CHECK_EQ_U64(bbd.mailbox_entries, 1048576);

        /* Strict rejection: oversize, negative, non-number, non-object,
         * unknown keys, and any admission/pump-tuning attempt. */
        static const char *bad[] = {
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"mailbox_entries\":1048577}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"demand_channel_bytes\":1099511627777}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"journal_entries\":-1}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"journal_entries\":\"many\"}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":7}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"bogus\":1}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"admit_remote_demand\":true}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"admit\":1}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"pump_turn_messages\":8}}}",
            "{\"listener\":{\"port\":1},\"budgets\":{\"cross_shard\":{"
            "\"pump_turn_bytes\":8}}}",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            moqr_cli_config_t r;
            MOQ_TEST_CHECK(parse(bad[i], &r, err, sizeof(err)) ==
                           MOQR_ERR_INVAL);
            MOQ_TEST_CHECK(strstr(err, "cross_shard") != NULL);
            if (i >= 5) {   /* unknown/admission/pump keys: the whitelist
                             * speaks first — "unknown key", never a type
                             * complaint about a rejected key's value. */
                MOQ_TEST_CHECK(strstr(err, "unknown key") != NULL);
            }
        }

        /* The shared resolver stays authoritative: an explicit channel byte
         * cap below the resolved log payload budget refuses through the
         * CLI capacity path. */
        moqr_cli_config_t low;
        MOQ_TEST_CHECK(parse(
            "{\"listener\":{\"port\":1,\"lanes\":2},\"budgets\":{"
            "\"log\":{\"max_bytes\":1048576},"
            "\"cross_shard\":{\"demand_channel_bytes\":1024}}}",
            &low, err, sizeof(err)) == MOQR_OK);   /* schema-valid... */
        moqr_cli_capacity_t cl;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&low, alloc, 0, &cl) ==
                       MOQR_ERR_INVAL);            /* ...resolver refuses */
        /* The same invariant holds at lanes=1 — accepting the object means
         * accepting VALID inert settings, never bypassing cross-field
         * validation. */
        moqr_cli_config_t low1;
        MOQ_TEST_CHECK(parse(
            "{\"listener\":{\"port\":1,\"lanes\":1},\"budgets\":{"
            "\"log\":{\"max_bytes\":1048576},"
            "\"cross_shard\":{\"demand_channel_bytes\":1024}}}",
            &low1, err, sizeof(err)) == MOQR_OK);
        moqr_cli_capacity_t cl1;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&low1, alloc, 0, &cl1) ==
                       MOQR_ERR_INVAL);
        /* The SHARED pre-command validator — the gate main() runs before
         * capacity AND both serve paths — refuses the same configs at both
         * lane counts, and passes valid ones. */
        MOQ_TEST_CHECK(moqr_cli_config_validate(&low1, alloc) ==
                       MOQR_ERR_INVAL);
        MOQ_TEST_CHECK(moqr_cli_config_validate(&low, alloc) ==
                       MOQR_ERR_INVAL);
        MOQ_TEST_CHECK(moqr_cli_config_validate(&full, alloc) == MOQR_OK);
        MOQ_TEST_CHECK(moqr_cli_config_validate(&emp, alloc) == MOQR_OK);

        /* lanes=1 accepts the object; the direct runtime composition is
         * untouched by cross-shard knobs. */
        moqr_cli_config_t l1p, l1c;
        MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1,\"lanes\":1}}",
                             &l1p, err, sizeof(err)) == MOQR_OK);
        MOQ_TEST_CHECK(parse(
            "{\"listener\":{\"port\":1,\"lanes\":1},\"budgets\":{"
            "\"cross_shard\":{\"mailbox_entries\":12,"
            "\"journal_entries\":11}}}",
            &l1c, err, sizeof(err)) == MOQR_OK);
        moqr_cli_capacity_t k1p, k1c;
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&l1p, alloc, 0, &k1p) ==
                       MOQR_OK);
        MOQ_TEST_CHECK(moqr_cli_describe_capacity(&l1c, alloc, 0, &k1c) ==
                       MOQR_OK);
        MOQ_TEST_CHECK_EQ_U64(k1p.total_bytes, k1c.total_bytes);
        MOQ_TEST_CHECK_EQ_U64(k1c.cross_shard_bytes, 0);
    }

    /* ---- auth: allow-all default, toy policy, strict failures ---- */

    /* Absent "auth" -> allow-all (no hook). */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1}}", &cfg, err,
                         sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_INT((int)cfg.auth.mode, (int)MOQR_CLI_AUTH_ALLOW_ALL);
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"allow_all\"}}",
              &cfg, err, sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_INT((int)cfg.auth.mode, (int)MOQR_CLI_AUTH_ALLOW_ALL);

    /* A toy policy parses: mode + default + one prefix rule. */
    MOQ_TEST_CHECK(
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
              "\"default\":\"allow\",\"rules\":[{\"action\":\"subscribe\","
              "\"namespace_prefix\":[\"live\"],\"decision\":\"deny\","
              "\"reason\":\"unscoped\"}]}}",
              &cfg, err, sizeof(err)) == MOQR_OK);
    MOQ_TEST_CHECK_EQ_INT((int)cfg.auth.mode, (int)MOQR_CLI_AUTH_TOY);
    MOQ_TEST_CHECK_EQ_SIZE(cfg.auth.toy.rule_count, (size_t)1);
    MOQ_TEST_CHECK(cfg.auth.toy.default_decision == MOQR_AUTH_ALLOW);
    MOQ_TEST_CHECK(cfg.auth.rules[0].action == MOQR_AUTH_SUBSCRIBE);
    MOQ_TEST_CHECK(cfg.auth.rules[0].decision == MOQR_AUTH_DENY);
    MOQ_TEST_CHECK_EQ_SIZE(cfg.auth.rules[0].ns_prefix.count, (size_t)1);

    /* Strict failures: every malformed auth doc is rejected loudly. */
    MOQ_TEST_CHECK(parse("{\"listener\":{\"port\":1},\"auth\":{}}", &cfg, err,
                         sizeof(err)) == MOQR_ERR_INVAL);   /* mode required  */
    MOQ_TEST_CHECK(   /* unknown auth key */
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
              "\"bogus\":1}}", &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(strstr(err, "auth") != NULL);
    MOQ_TEST_CHECK(   /* toy needs an explicit default */
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
              "\"rules\":[]}}", &cfg, err, sizeof(err)) == MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(   /* allow_all rejects default/rules */
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"allow_all\","
              "\"default\":\"deny\"}}", &cfg, err, sizeof(err)) ==
        MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(   /* unknown action */
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
              "\"default\":\"deny\",\"rules\":[{\"action\":\"bogus\","
              "\"decision\":\"allow\"}]}}", &cfg, err, sizeof(err)) ==
        MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(   /* DEFER is not authorable in a rule */
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
              "\"default\":\"deny\",\"rules\":[{\"action\":\"subscribe\","
              "\"decision\":\"defer\"}]}}", &cfg, err, sizeof(err)) ==
        MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(   /* nor as the default */
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
              "\"default\":\"defer\"}}", &cfg, err, sizeof(err)) ==
        MOQR_ERR_INVAL);
    MOQ_TEST_CHECK(   /* unknown reason */
        parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
              "\"default\":\"deny\",\"rules\":[{\"action\":\"subscribe\","
              "\"decision\":\"deny\",\"reason\":\"bogus\"}]}}", &cfg, err,
              sizeof(err)) == MOQR_ERR_INVAL);

    /* build_core installs the toy hook and it ENFORCES — not just parsed. */
    {
        moqr_cli_config_t ac;
        MOQ_TEST_CHECK(
            parse("{\"listener\":{\"port\":1},\"auth\":{\"mode\":\"toy\","
                  "\"default\":\"allow\",\"rules\":[{\"action\":\"subscribe\","
                  "\"namespace_prefix\":[\"live\"],\"decision\":\"deny\","
                  "\"reason\":\"unscoped\"}]}}",
                  &ac, err, sizeof(err)) == MOQR_OK);
        const moq_alloc_t *alloc = moq_alloc_default();
        moqr_trace_t *trace = NULL;
        moqr_core_t *core = NULL;
        MOQ_TEST_CHECK(moqr_cli_build_core(&ac, alloc, &trace, &core) ==
                       MOQR_OK);

        moqr_auth_request_t req;
        moqr_auth_verdict_t v;
        moq_bytes_t live[1];
        moq_bytes_t vod[1];
        live[0].data = (const uint8_t *)"live";
        live[0].len = 4;
        vod[0].data = (const uint8_t *)"vod";
        vod[0].len = 3;

        /* subscribe under "live" -> rule -> DENY(unscoped) */
        memset(&req, 0, sizeof(req));
        req.struct_size = (uint32_t)sizeof(req);
        req.action = MOQR_AUTH_SUBSCRIBE;
        req.ns.parts = live;
        req.ns.count = 1;
        moqr_core_authorize(core, &req, &v);
        MOQ_TEST_CHECK(v.decision == MOQR_AUTH_DENY);
        MOQ_TEST_CHECK(v.reason == MOQR_AUTH_REASON_UNSCOPED);

        /* subscribe elsewhere -> default ALLOW (proves the hook, not a stub) */
        req.ns.parts = vod;
        req.ns.count = 1;
        moqr_core_authorize(core, &req, &v);
        MOQ_TEST_CHECK(v.decision == MOQR_AUTH_ALLOW);

        moqr_core_destroy(core);
        moqr_trace_destroy(trace);
    }

    failures += test_snapshot_protocol();
    failures += test_snapshot_render_gate();
    failures += test_snapshot_protocol_mt();
    failures += test_snapshot_coord_production();
    failures += test_snapshot_coord_failures();

    MOQ_TEST_PASS("relay_cli_config");
    return failures == 0 ? 0 : 1; /* exit status truncates to 8 bits */
}
