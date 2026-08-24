/*
 * SimPair trace record contract: the frozen v0 prefix, and the appended
 * detail fields.
 *
 * Two obligations, deliberately proven separately.
 *
 * COMPATIBILITY. The prefix through `result` is frozen: for every kind and
 * subtype it carries exactly what the pre-append producer emitted, so the
 * twin-run determinism suites that hash it see byte-identical results. A
 * frozen-v0 consumer and hard numeric prefix hashes pin that here, rather
 * than leaving it to inspection of the producers.
 *
 * DETAIL. `stream_ref`, `detail_bytes`, `error_code` and `fin` carry the facts
 * the prefix cannot: which stream an action or delivery belongs to, the bytes
 * the legacy field never exposed (a SEND_DATA payload, a CLOSE_SESSION
 * reason, a bidi/uni-control data span), and the terminal code of every
 * code-bearing action -- uniformly, instead of by subtype.
 *
 * Expectations are ABSOLUTE and declared per row: each record is compared
 * against an independently written image, never against the other producer's
 * output. ACTION and FAULT_DROP are required to agree only AFTER each has
 * matched its own declared image, so a shared normalizer that is uniformly
 * wrong cannot pass by agreeing with itself.
 *
 * Deterministic and socket-free: one thread, no wall time.
 *
 * Usage: test_sim_trace_contract
 */
#include <moq/moq.h>
#include <moq/sim.h>

#include "simpair_internal.h"   /* trace_action/_fault_drop/_input; sp fields */
#include "test_support.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECKN(name, expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL[%s]: %s:%d: %s\n", (name), __FILE__, __LINE__, \
                #expr); \
        failures++; \
    } \
} while (0)

/* Named per (class, field) so the inventory decomposes by what is missing. */
static void checkf(const char *cls, const char *field, bool ok)
{
    char n[128];
    if (ok) return;
    snprintf(n, sizeof(n), "%s.%s", cls, field);
    fprintf(stderr, "FAIL[%s]\n", n);
    failures++;
}

/* ===================================================================== *
 * The legacy v0 layout, copied INDEPENDENTLY from the record as it was
 * published before the append. Its sizeof is the authority for the floor:
 * `MOQ_SIM_TRACE_RECORD_V0_SIZE` must equal it exactly, padding included.
 * Declared here rather than derived from the current struct so a future
 * reordering cannot silently redefine what "v0" meant.
 * ===================================================================== */

typedef struct {
    uint32_t                    struct_size;
    moq_sim_trace_kind_t        kind;
    uint64_t                    seed;
    uint64_t                    step;
    uint64_t                    now_us;
    moq_perspective_t           from;
    moq_perspective_t           to;
    moq_action_kind_t           action_kind;
    moq_sim_trace_input_kind_t  input_kind;
    moq_bytes_t                 bytes;
    uint64_t                    code;
    size_t                      count;
    moq_result_t                result;
} legacy_v0_record_t;

_Static_assert(sizeof(legacy_v0_record_t) == MOQ_SIM_TRACE_RECORD_V0_SIZE,
               "MOQ_SIM_TRACE_RECORD_V0_SIZE must equal sizeof the record as "
               "first published, trailing padding included");
_Static_assert(offsetof(moq_sim_trace_record_t, stream_ref) ==
               MOQ_SIM_TRACE_RECORD_V0_SIZE,
               "the first appended field must begin exactly at the v0 floor");

/* ===================================================================== *
 * Captured record (deep copy: both spans are borrowed for the callback)
 * ===================================================================== */

#define CAP_MAX     256
#define CAP_BYTES   512

typedef struct {
    /* frozen v0 prefix */
    uint32_t                   struct_size;
    moq_sim_trace_kind_t       kind;
    uint64_t                   seed, step, now_us;
    moq_perspective_t          from, to;
    moq_action_kind_t          action_kind;
    moq_sim_trace_input_kind_t input_kind;
    size_t                     bytes_len;
    bool                       bytes_null;
    uint8_t                    bytes[CAP_BYTES];
    uint64_t                   code;
    size_t                     count;
    moq_result_t               result;
    /* appended detail */
    uint64_t                   stream_ref;
    size_t                     detail_len;
    bool                       detail_null;
    uint8_t                    detail[CAP_BYTES];
    uint64_t                   error_code;
    bool                       fin;
} cap_t;

typedef struct {
    cap_t    recs[CAP_MAX];
    size_t   n;
    bool     overflow;
    bool     incomparable;   /* an invalid span shape was seen */
    uint64_t hash;          /* v0-prefix hash, the shape the suites use */
} cap_log_t;

static void cap_reset(cap_log_t *l)
{
    memset(l, 0, sizeof(*l));
    l->hash = 1469598103934665603ULL;
}

/* The prefix hash exactly as the 19 in-tree twin-run consumers compute it. */
static void cap_hash_prefix(cap_log_t *l, const moq_sim_trace_record_t *r)
{
    uint64_t h = l->hash;
    h ^= r->seed;                  h *= 0x100000001B3ULL;
    h ^= r->step;                  h *= 0x100000001B3ULL;
    h ^= r->now_us;                h *= 0x100000001B3ULL;
    h ^= (uint64_t)r->kind;        h *= 0x100000001B3ULL;
    h ^= (uint64_t)r->from;        h *= 0x100000001B3ULL;
    h ^= (uint64_t)r->to;          h *= 0x100000001B3ULL;
    h ^= (uint64_t)r->action_kind; h *= 0x100000001B3ULL;
    h ^= (uint64_t)r->input_kind;  h *= 0x100000001B3ULL;
    h ^= (uint64_t)r->result;      h *= 0x100000001B3ULL;
    h ^= r->code;                  h *= 0x100000001B3ULL;
    h ^= r->bytes.len;             h *= 0x100000001B3ULL;
    if (r->bytes.data && r->bytes.len > 0)
        for (size_t i = 0; i < r->bytes.len; i++) {
            h ^= r->bytes.data[i]; h *= 0x100000001B3ULL;
        }
    l->hash = h;
}

static void cap_fn(void *ctx, const moq_sim_trace_record_t *r)
{
    cap_log_t *l = (cap_log_t *)ctx;

    cap_hash_prefix(l, r);
    if (l->n >= CAP_MAX) { l->overflow = true; return; }

    cap_t *c = &l->recs[l->n++];
    memset(c, 0, sizeof(*c));
    c->struct_size = r->struct_size;
    c->kind = r->kind;
    c->seed = r->seed; c->step = r->step; c->now_us = r->now_us;
    c->from = r->from; c->to = r->to;
    c->action_kind = r->action_kind;
    c->input_kind = r->input_kind;
    c->bytes_len = r->bytes.len;
    c->bytes_null = (r->bytes.data == NULL);
    /* The legacy span is bounds-checked exactly like the appended one. An
     * oversize span, or a nonzero length with a NULL pointer, makes the
     * capture INCOMPARABLE (sticky) -- it must never turn into a silently
     * zero-filled comparison that the row would then "match". */
    if (r->bytes.len > CAP_BYTES) l->incomparable = true;
    else if (r->bytes.len > 0 && r->bytes.data == NULL) l->incomparable = true;
    else if (r->bytes.data && r->bytes.len > 0)
        memcpy(c->bytes, r->bytes.data, r->bytes.len);
    c->code = r->code;
    c->count = r->count;
    c->result = r->result;

    /* Appended fields are guarded INDEPENDENTLY, each by its own extent --
     * the documented per-field ABI rule. An all-or-nothing guard keyed on the
     * last field would be conservative but would not exercise the contract,
     * and would hide a bad intermediate boundary. */
#define ADVERTISED(f) \
    (r->struct_size >= offsetof(moq_sim_trace_record_t, f) + sizeof(r->f))

    if (ADVERTISED(stream_ref))
        c->stream_ref = r->stream_ref._v;
    if (ADVERTISED(detail_bytes)) {
        c->detail_len = r->detail_bytes.len;
        c->detail_null = (r->detail_bytes.data == NULL);
        /* bounds-check before touching the borrowed span */
        if (r->detail_bytes.len > CAP_BYTES) l->incomparable = true;
        else if (r->detail_bytes.len > 0 && r->detail_bytes.data == NULL)
            l->incomparable = true;
        else if (r->detail_bytes.data && r->detail_bytes.len > 0)
            memcpy(c->detail, r->detail_bytes.data, r->detail_bytes.len);
    }
    if (ADVERTISED(error_code))
        c->error_code = r->error_code;
    if (ADVERTISED(fin))
        c->fin = r->fin;
#undef ADVERTISED
}

/* ===================================================================== *
 * Declared image + comparison
 * ===================================================================== */

typedef struct {
    const char       *cls;          /* diagnostic class, e.g. "action.send_data" */
    /* prefix */
    moq_sim_trace_kind_t kind;
    moq_action_kind_t    action_kind;
    size_t               bytes_len;
    const uint8_t       *bytes;
    uint64_t             code;
    size_t               count;
    /* appended detail */
    uint64_t             stream_ref;
    size_t               detail_len;
    const uint8_t       *detail;
    uint64_t             error_code;
    bool                 fin;
    /* The two producers are NOT identical in the LEGACY prefix today:
     * trace_fault_drop has no CLOSE_SESSION branch, so a dropped close
     * carries code 0 where the action carries the close code. The frozen
     * prefix rule preserves that divergence; only the APPENDED detail is
     * required to match between them. */
    bool                 drop_prefix_differs;
    uint64_t             drop_code;
} image_t;

static void check_prefix(const image_t *w, const cap_t *g)
{
    checkf(w->cls, "prefix.kind",        (int)g->kind == (int)w->kind);
    checkf(w->cls, "prefix.action_kind", (int)g->action_kind == (int)w->action_kind);
    checkf(w->cls, "prefix.bytes_len",   g->bytes_len == w->bytes_len);
    checkf(w->cls, "prefix.code",        g->code == w->code);
    checkf(w->cls, "prefix.count",       g->count == w->count);
    if (w->bytes_len > 0)
        checkf(w->cls, "prefix.bytes",
               !g->bytes_null && memcmp(g->bytes, w->bytes, w->bytes_len) == 0);
    else
        checkf(w->cls, "prefix.bytes_absent", g->bytes_len == 0);
}

static void check_detail(const image_t *w, const cap_t *g)
{
    checkf(w->cls, "detail.stream_ref", g->stream_ref == w->stream_ref);
    checkf(w->cls, "detail.error_code", g->error_code == w->error_code);
    checkf(w->cls, "detail.fin",        g->fin == w->fin);
    checkf(w->cls, "detail.bytes_len",  g->detail_len == w->detail_len);
    if (w->detail_len > 0)
        checkf(w->cls, "detail.bytes",
               !g->detail_null &&
               memcmp(g->detail, w->detail, w->detail_len) == 0);
    else
        checkf(w->cls, "detail.bytes_absent", g->detail_len == 0);
}

/* True when the two captures carry identical appended detail. */
static bool detail_equal(const cap_t *a, const cap_t *b)
{
    if (a->stream_ref != b->stream_ref) return false;
    if (a->error_code != b->error_code) return false;
    if (a->fin != b->fin) return false;
    if (a->detail_len != b->detail_len) return false;
    if (a->detail_null != b->detail_null) return false;
    return a->detail_len == 0 ||
           memcmp(a->detail, b->detail, a->detail_len) == 0;
}

/* ===================================================================== *
 * 1. ACTION / FAULT_DROP: one declared image per table row
 * ===================================================================== */

static const uint8_t K_CTRL[]   = { 0x40, 0x41, 0x42 };
static const uint8_t K_DGRAM[]  = { 0x50, 0x51 };
static const uint8_t K_HDR[]    = { 0x60, 0x61, 0x62, 0x63 };
static const uint8_t K_PAYLOAD[]= { 0x70, 0x71, 0x72, 0x73, 0x74 };
static const uint8_t K_BIDI[]   = { 0x80, 0x81 };
static const uint8_t K_UNI[]    = { 0x90, 0x91, 0x92 };
static const char    K_REASON[] = "declared close reason";

#define REF_DATA   0x111ULL
#define REF_BIDI   0x222ULL
#define REF_UNI    0x333ULL
#define CODE_CLOSE 0x0AULL
#define CODE_RESET 0x1BULL
#define CODE_STOP  0x2CULL
#define CODE_BIDI  0x3DULL

/* A rig whose only job is to give the internal producers a `sp` with our
 * capture callback installed. No handshake is run: these rows are about the
 * record the producer emits for a GIVEN action, not about protocol flow. */
typedef struct {
    moq_simpair_t *sp;
    cap_log_t      log;
} arig_t;

static int arig_up(arig_t *r, moq_version_t v)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;

    cap_reset(&r->log);
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0xAC7u;
    cfg.version = v;
    cfg.trace_fn = cap_fn;
    cfg.trace_ctx = &r->log;
    r->sp = NULL;
    return moq_simpair_create(&cfg, &r->sp) == MOQ_OK && r->sp ? 0 : -1;
}

static void arig_down(arig_t *r)
{
    if (r->sp) moq_simpair_destroy(r->sp);
    r->sp = NULL;
}

/*
 * Emit one action through BOTH producers and check each against the declared
 * image independently; only then require the two detail images to agree.
 */
static void run_action_row(arig_t *r, const moq_action_t *a, const image_t *w)
{
    size_t base = r->log.n;

    trace_action(r->sp, MOQ_PERSPECTIVE_CLIENT, MOQ_PERSPECTIVE_SERVER, a);
    trace_fault_drop(r->sp, MOQ_PERSPECTIVE_CLIENT, MOQ_PERSPECTIVE_SERVER, a);

    if (r->log.n != base + 2 || r->log.overflow) {
        checkf(w->cls, "emitted_two_records", false);
        return;
    }

    const cap_t *act = &r->log.recs[base];
    const cap_t *drp = &r->log.recs[base + 1];

    image_t wa = *w; wa.kind = MOQ_SIM_TRACE_ACTION;
    image_t wd = *w; wd.kind = MOQ_SIM_TRACE_FAULT_DROP;
    if (w->drop_prefix_differs) wd.code = w->drop_code;

    check_prefix(&wa, act);
    check_detail(&wa, act);
    check_prefix(&wd, drp);
    check_detail(&wd, drp);

    /* Only meaningful once both matched their own declared image: a shared
     * normalizer that is uniformly wrong agrees with itself. */
    checkf(w->cls, "action_eq_fault_drop", detail_equal(act, drp));
}

static void t_action_rows(void)
{
    int before = failures;
    arig_t r;
    moq_action_t a;
    moq_rcbuf_t *payload = NULL;

    printf("ACTION-ROWS:\n");
    CHECKN("rows.rig", arig_up(&r, MOQ_VERSION_DRAFT_16) == 0);
    if (!r.sp) return;
    CHECKN("rows.payload",
           moq_rcbuf_create(moq_alloc_default(), K_PAYLOAD,
                            sizeof(K_PAYLOAD), &payload) == MOQ_OK);

    /* -- SEND_CONTROL: legacy bytes only, no detail ------------------- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_SEND_CONTROL;
    a.u.send_control.data = K_CTRL;
    a.u.send_control.len = sizeof(K_CTRL);
    run_action_row(&r, &a, &(image_t){
        .cls = "send_control", .action_kind = MOQ_ACTION_SEND_CONTROL,
        .bytes = K_CTRL, .bytes_len = sizeof(K_CTRL) });

    /* -- SEND_DATAGRAM ------------------------------------------------ */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_SEND_DATAGRAM;
    a.u.send_datagram.data = K_DGRAM;
    a.u.send_datagram.len = sizeof(K_DGRAM);
    run_action_row(&r, &a, &(image_t){
        .cls = "send_datagram", .action_kind = MOQ_ACTION_SEND_DATAGRAM,
        .bytes = K_DGRAM, .bytes_len = sizeof(K_DGRAM) });

    /* -- SEND_DATA: legacy bytes = header, legacy count = payload LEN,
     *    detail_bytes = the payload itself, ref + FIN in detail -------- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_SEND_DATA;
    a.u.send_data.stream_ref = moq_stream_ref_from_u64(REF_DATA);
    memcpy(a.u.send_data.header, K_HDR, sizeof(K_HDR));
    a.u.send_data.header_len = (uint8_t)sizeof(K_HDR);
    a.u.send_data.payload = payload;
    a.u.send_data.fin = true;
    run_action_row(&r, &a, &(image_t){
        .cls = "send_data", .action_kind = MOQ_ACTION_SEND_DATA,
        .bytes = K_HDR, .bytes_len = sizeof(K_HDR),
        .count = sizeof(K_PAYLOAD),
        .stream_ref = REF_DATA,
        .detail = K_PAYLOAD, .detail_len = sizeof(K_PAYLOAD),
        .fin = true });

    /* -- CLOSE_SESSION: reason in detail_bytes, code DUPLICATED -------- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_CLOSE_SESSION;
    a.u.close_session.code = CODE_CLOSE;
    a.u.close_session.reason.data = (const uint8_t *)K_REASON;
    a.u.close_session.reason.len = sizeof(K_REASON) - 1;
    run_action_row(&r, &a, &(image_t){
        .cls = "close_session", .action_kind = MOQ_ACTION_CLOSE_SESSION,
        .code = CODE_CLOSE,
        .detail = (const uint8_t *)K_REASON, .detail_len = sizeof(K_REASON) - 1,
        .error_code = CODE_CLOSE,
        /* frozen: trace_fault_drop has never carried the close code */
        .drop_prefix_differs = true, .drop_code = 0 });

    /* -- OPEN_BIDI_STREAM --------------------------------------------- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_OPEN_BIDI_STREAM;
    a.u.open_bidi_stream.stream_ref = moq_stream_ref_from_u64(REF_BIDI);
    a.u.open_bidi_stream.data = K_BIDI;
    a.u.open_bidi_stream.len = sizeof(K_BIDI);
    a.u.open_bidi_stream.fin = true;
    run_action_row(&r, &a, &(image_t){
        .cls = "open_bidi", .action_kind = MOQ_ACTION_OPEN_BIDI_STREAM,
        .stream_ref = REF_BIDI,
        .detail = K_BIDI, .detail_len = sizeof(K_BIDI), .fin = true });

    /* -- SEND_BIDI_STREAM --------------------------------------------- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_SEND_BIDI_STREAM;
    a.u.send_bidi_stream.stream_ref = moq_stream_ref_from_u64(REF_BIDI);
    a.u.send_bidi_stream.data = K_BIDI;
    a.u.send_bidi_stream.len = sizeof(K_BIDI);
    a.u.send_bidi_stream.fin = false;
    run_action_row(&r, &a, &(image_t){
        .cls = "send_bidi", .action_kind = MOQ_ACTION_SEND_BIDI_STREAM,
        .stream_ref = REF_BIDI,
        .detail = K_BIDI, .detail_len = sizeof(K_BIDI), .fin = false });

    /* -- CLOSE_BIDI_STREAM: ref only ----------------------------------- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_CLOSE_BIDI_STREAM;
    a.u.close_bidi_stream.stream_ref = moq_stream_ref_from_u64(REF_BIDI);
    run_action_row(&r, &a, &(image_t){
        .cls = "close_bidi", .action_kind = MOQ_ACTION_CLOSE_BIDI_STREAM,
        .stream_ref = REF_BIDI });

    /* -- OPEN_UNI_CONTROL: no FIN MEMBER exists, so fin stays false ---- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_OPEN_UNI_CONTROL;
    a.u.open_uni_control.stream_ref = moq_stream_ref_from_u64(REF_UNI);
    a.u.open_uni_control.data = K_UNI;
    a.u.open_uni_control.len = sizeof(K_UNI);
    run_action_row(&r, &a, &(image_t){
        .cls = "open_uni_control", .action_kind = MOQ_ACTION_OPEN_UNI_CONTROL,
        .stream_ref = REF_UNI,
        .detail = K_UNI, .detail_len = sizeof(K_UNI), .fin = false });

    /* -- SEND_UNI_CONTROL: has a FIN member ---------------------------- */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_SEND_UNI_CONTROL;
    a.u.send_uni_control.stream_ref = moq_stream_ref_from_u64(REF_UNI);
    a.u.send_uni_control.data = K_UNI;
    a.u.send_uni_control.len = sizeof(K_UNI);
    a.u.send_uni_control.fin = true;
    run_action_row(&r, &a, &(image_t){
        .cls = "send_uni_control", .action_kind = MOQ_ACTION_SEND_UNI_CONTROL,
        .stream_ref = REF_UNI,
        .detail = K_UNI, .detail_len = sizeof(K_UNI), .fin = true });

    /* -- RESET_DATA / STOP_DATA: legacy code kept, duplicated in detail - */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_RESET_DATA;
    a.u.reset_data.stream_ref = moq_stream_ref_from_u64(REF_DATA);
    a.u.reset_data.error_code = CODE_RESET;
    run_action_row(&r, &a, &(image_t){
        .cls = "reset_data", .action_kind = MOQ_ACTION_RESET_DATA,
        .code = CODE_RESET, .stream_ref = REF_DATA,
        .error_code = CODE_RESET });

    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_STOP_DATA;
    a.u.stop_data.stream_ref = moq_stream_ref_from_u64(REF_DATA);
    a.u.stop_data.error_code = CODE_STOP;
    run_action_row(&r, &a, &(image_t){
        .cls = "stop_data", .action_kind = MOQ_ACTION_STOP_DATA,
        .code = CODE_STOP, .stream_ref = REF_DATA,
        .error_code = CODE_STOP });

    /* -- bidi reset/stop/abort: NO legacy code today, detail carries it - */
    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_RESET_BIDI_STREAM;
    a.u.reset_bidi_stream.stream_ref = moq_stream_ref_from_u64(REF_BIDI);
    a.u.reset_bidi_stream.error_code = CODE_BIDI;
    run_action_row(&r, &a, &(image_t){
        .cls = "reset_bidi", .action_kind = MOQ_ACTION_RESET_BIDI_STREAM,
        .stream_ref = REF_BIDI, .error_code = CODE_BIDI });

    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_STOP_BIDI_STREAM;
    a.u.stop_bidi_stream.stream_ref = moq_stream_ref_from_u64(REF_BIDI);
    a.u.stop_bidi_stream.error_code = CODE_BIDI;
    run_action_row(&r, &a, &(image_t){
        .cls = "stop_bidi", .action_kind = MOQ_ACTION_STOP_BIDI_STREAM,
        .stream_ref = REF_BIDI, .error_code = CODE_BIDI });

    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_ABORT_BIDI_STREAM;
    a.u.abort_bidi_stream.stream_ref = moq_stream_ref_from_u64(REF_BIDI);
    a.u.abort_bidi_stream.error_code = CODE_BIDI;
    run_action_row(&r, &a, &(image_t){
        .cls = "abort_bidi", .action_kind = MOQ_ACTION_ABORT_BIDI_STREAM,
        .stream_ref = REF_BIDI, .error_code = CODE_BIDI });

    if (payload) moq_rcbuf_decref(payload);
    arig_down(&r);
    if (failures == before) printf("PASS: action_rows\n");
}

/* ===================================================================== *
 * 2. Close-reason discriminators
 * ===================================================================== */

static void emit_close(arig_t *r, uint64_t code, const char *reason,
                       size_t reason_len)
{
    moq_action_t a;

    memset(&a, 0, sizeof(a));
    a.kind = MOQ_ACTION_CLOSE_SESSION;
    a.u.close_session.code = code;
    a.u.close_session.reason.data = (const uint8_t *)reason;
    a.u.close_session.reason.len = reason_len;
    trace_action(r->sp, MOQ_PERSPECTIVE_CLIENT, MOQ_PERSPECTIVE_SERVER, &a);
}

static void t_close_reason(void)
{
    int before = failures;
    arig_t r;
    static const char r1[] = "first reason";
    static const char r2[] = "second reason!";

    printf("CLOSE-REASON:\n");
    CHECKN("close.rig", arig_up(&r, MOQ_VERSION_DRAFT_16) == 0);
    if (!r.sp) return;

    /* same code, different non-empty reasons -> distinguishable */
    emit_close(&r, CODE_CLOSE, r1, sizeof(r1) - 1);
    emit_close(&r, CODE_CLOSE, r2, sizeof(r2) - 1);
    /* empty (NULL) reason, same code */
    emit_close(&r, CODE_CLOSE, NULL, 0);

    if (r.log.n != 3 || r.log.overflow) {
        CHECKN("close.three_records", false);
        arig_down(&r);
        return;
    }
    const cap_t *a = &r.log.recs[0], *b = &r.log.recs[1], *c = &r.log.recs[2];

    /* the legacy prefix cannot tell them apart -- that is why detail exists */
    CHECKN("close.prefix_indistinguishable",
           a->code == b->code && b->code == c->code &&
           a->bytes_len == 0 && b->bytes_len == 0 && c->bytes_len == 0);

    checkf("close.first",  "detail.bytes_len", a->detail_len == sizeof(r1) - 1);
    checkf("close.first",  "detail.bytes",
           a->detail_len == sizeof(r1) - 1 &&
           memcmp(a->detail, r1, sizeof(r1) - 1) == 0);
    checkf("close.second", "detail.bytes_len", b->detail_len == sizeof(r2) - 1);
    checkf("close.second", "detail.bytes",
           b->detail_len == sizeof(r2) - 1 &&
           memcmp(b->detail, r2, sizeof(r2) - 1) == 0);
    checkf("close.empty",  "detail.bytes_absent", c->detail_len == 0);
    checkf("close.empty",  "detail.bytes_null",   c->detail_null);

    /* same code, different reason: the two records differ in detail */
    checkf("close.discriminates", "reasons_differ",
           !(a->detail_len == b->detail_len &&
             memcmp(a->detail, b->detail, a->detail_len) == 0));

    /* the deep copy taken inside the callback is what we compared: the spans
     * themselves are borrowed only for the callback's duration */
    checkf("close.first",  "detail.error_code", a->error_code == CODE_CLOSE);
    checkf("close.empty",  "detail.error_code", c->error_code == CODE_CLOSE);

    arig_down(&r);
    if (failures == before) printf("PASS: close_reason\n");
}

/* ===================================================================== *
 * 3. struct_size guards, both directions
 * ===================================================================== */

/*
 * Every intermediate advertised boundary, not just v0 and full size: each
 * synthetic record ends exactly at one appended field, and the test requires
 * every covered field to be read and every LATER field -- deliberately filled
 * with garbage -- not to be.
 */
typedef struct {
    const char *name;
    size_t      advertised;
    bool        want_ref, want_detail, want_code, want_fin;
} ss_case_t;

static void run_ss_case(const ss_case_t *k)
{
    cap_log_t log;
    moq_sim_trace_record_t rec;
    static const uint8_t GARBAGE[] = { 0xEE, 0xEE, 0xEE, 0xEE };
    char cls[96];

    snprintf(cls, sizeof(cls), "ss.%s", k->name);
    cap_reset(&log);
    memset(&rec, 0, sizeof(rec));
    rec.struct_size = (uint32_t)k->advertised;
    rec.kind = MOQ_SIM_TRACE_ACTION;
    rec.action_kind = MOQ_ACTION_RESET_DATA;
    rec.code = 7;                                   /* prefix, always read */
    /* every appended field carries a value a truncated consumer must NOT see */
    rec.stream_ref = moq_stream_ref_from_u64(0xDEADBEEF);
    rec.detail_bytes.data = GARBAGE;
    rec.detail_bytes.len = sizeof(GARBAGE);
    rec.error_code = 0xFEEDu;
    rec.fin = true;

    cap_fn(&log, &rec);
    if (log.n != 1) { checkf(cls, "one_record", false); return; }
    const cap_t *g = &log.recs[0];

    checkf(cls, "prefix.code", g->code == 7);
    checkf(cls, "stream_ref",
           g->stream_ref == (k->want_ref ? 0xDEADBEEFu : 0u));
    checkf(cls, "detail_bytes",
           g->detail_len == (k->want_detail ? sizeof(GARBAGE) : 0u));
    checkf(cls, "error_code",
           g->error_code == (k->want_code ? 0xFEEDu : 0u));
    checkf(cls, "fin", g->fin == (k->want_fin ? true : false));
}

static void t_struct_size(void)
{
    int before = failures;

    printf("STRUCT-SIZE:\n");

    /* the floor is the legacy layout's SIZE, padding included, and is exactly
     * where the first appended field begins (also asserted at compile time) */
    CHECKN("v0.floor_equals_legacy_sizeof",
           (size_t)MOQ_SIM_TRACE_RECORD_V0_SIZE == sizeof(legacy_v0_record_t));
    CHECKN("v0.floor_equals_first_appended_offset",
           (size_t)MOQ_SIM_TRACE_RECORD_V0_SIZE ==
               offsetof(moq_sim_trace_record_t, stream_ref));
    CHECKN("v0.floor_above_end_of_result",
           (size_t)MOQ_SIM_TRACE_RECORD_V0_SIZE >=
               offsetof(moq_sim_trace_record_t, result) + sizeof(moq_result_t));
    CHECKN("v0.floor_below_full",
           (size_t)MOQ_SIM_TRACE_RECORD_V0_SIZE <
               sizeof(moq_sim_trace_record_t));

    const ss_case_t CASES[] = {
        { "v0_only",       MOQ_SIM_TRACE_RECORD_V0_SIZE,
          false, false, false, false },
        { "thru_ref",      offsetof(moq_sim_trace_record_t, stream_ref) +
                           sizeof(moq_stream_ref_t),
          true,  false, false, false },
        { "thru_detail",   offsetof(moq_sim_trace_record_t, detail_bytes) +
                           sizeof(moq_bytes_t),
          true,  true,  false, false },
        { "thru_code",     offsetof(moq_sim_trace_record_t, error_code) +
                           sizeof(uint64_t),
          true,  true,  true,  false },
        { "thru_fin",      offsetof(moq_sim_trace_record_t, fin) + sizeof(bool),
          true,  true,  true,  true },
        { "full",          sizeof(moq_sim_trace_record_t),
          true,  true,  true,  true },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++)
        run_ss_case(&CASES[i]);

    if (failures == before) printf("PASS: struct_size\n");
}

/* ===================================================================== *
 * 4. INPUT records over live delivery: the full required matrix
 *
 * Every row is driven through the real delivery machinery
 * (deliver_or_delay_*), on a REAL established pair whose stream and bidi maps
 * were created by real traffic. The expected ref is the TARGET SESSION'S LOCAL
 * MAPPED REF -- the ref the delivery itself passes to moq_session_on_* -- read
 * from the LIVE map (sim_stream_map_find / sim_bidi_find_by_* plus the map
 * arrays) and required to match EXACTLY. That is the receiver's ref for the
 * bytes and RESET/BIDI_STOP rows, and the SENDER's ref for the DATA_STOP row,
 * which travels back toward the sender: "receiver side" is not a universal
 * synonym here. Inequality with the far-end ref is kept only as a secondary
 * discriminator for today's allocator; it is not the authority and
 * non-equality is not asserted as part of the public contract.
 *
 * The expected value is captured BEFORE the row is delivered, because a
 * RESET/STOP retires the map entry and would invalidate the lookup afterwards.
 * ===================================================================== */

typedef struct {
    moq_simpair_t *sp;
    cap_log_t      log;
} lrig_t;

static void drain_events(moq_session_t *s)
{
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) > 0) moq_event_cleanup(&ev);
}

static int lrig_up(lrig_t *r, bool delay_all)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;

    cap_reset(&r->log);
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x1u;
    cfg.version = MOQ_VERSION_DRAFT_18;
    cfg.trace_fn = cap_fn;
    cfg.trace_ctx = &r->log;
    if (delay_all) {
        cfg.fault_per_mille = 1000;              /* every eligible delivery */
        cfg.fault_flags = MOQ_SIM_FAULT_DELAY;
    }
    r->sp = NULL;
    if (moq_simpair_create(&cfg, &r->sp) != MOQ_OK || !r->sp) return -1;
    moq_simpair_start(r->sp);
    if (moq_simpair_run_until_quiescent(r->sp, 32, NULL) != MOQ_OK) return -1;
    drain_events(moq_simpair_client(r->sp));
    drain_events(moq_simpair_server(r->sp));
    if (delay_all) moq_simpair_enable_faults(r->sp);   /* after the handshake */
    return 0;
}

/* Settle: pump, then advance VIRTUAL time to the next deadline while delayed
 * entries remain. No sleeps, no wall time. */
/*
 * Settlement is an ASSERTED step, not a best effort. Every pump and every
 * virtual-time advance must return MOQ_OK, the loop must converge strictly
 * inside its bound, and the delayed queue must be empty on return -- otherwise
 * a row would be inspecting a partial trace with no name for the transport
 * failure. Virtual time only: no sleeps, no wall-clock deadline.
 */
#define LRIG_SETTLE_BOUND 64

static void lrig_settle(lrig_t *r, const char *cls, const char *arm)
{
    char f[96];
    bool pumps_ok = true, advances_ok = true, converged = false;
    int i;

    for (i = 0; i < LRIG_SETTLE_BOUND; i++) {
        if (moq_simpair_run_until_quiescent(r->sp, 32, NULL) != MOQ_OK)
            pumps_ok = false;
        if (moq_simpair_delayed_count(r->sp) == 0) { converged = true; break; }
        uint64_t d = moq_simpair_next_deadline_us(r->sp);
        /* No delayed entry may be unschedulable: a queue that is non-empty
         * with no deadline is a stall, not a convergence. */
        if (d == UINT64_MAX) break;
        if (d < moq_simpair_now_us(r->sp)) d = moq_simpair_now_us(r->sp);
        if (moq_simpair_advance_to(r->sp, d) != MOQ_OK) advances_ok = false;
    }
    if (moq_simpair_run_until_quiescent(r->sp, 32, NULL) != MOQ_OK)
        pumps_ok = false;

    snprintf(f, sizeof(f), "settle.%s.pumps_ok", arm);
    checkf(cls, f, pumps_ok);
    snprintf(f, sizeof(f), "settle.%s.advances_ok", arm);
    checkf(cls, f, advances_ok);
    snprintf(f, sizeof(f), "settle.%s.converged", arm);
    checkf(cls, f, converged && i < LRIG_SETTLE_BOUND);
    snprintf(f, sizeof(f), "settle.%s.delayed_drained", arm);
    checkf(cls, f, moq_simpair_delayed_count(r->sp) == 0);
}

/*
 * A DELIBERATELY UNASSERTED settle, used ONLY by the HARD fault-injection and
 * mutation rigs (mutate / truncate / reorder / inject-reset / inject-stop /
 * inject-close). There a delivery may legitimately fail or close the session,
 * so requiring MOQ_OK would assert the absence of the very behaviour those
 * rigs exist to produce; the rows they check are record-shape rows, not
 * delivery-outcome rows.
 *
 * It is NOT used by the delay-only rigs: delay must PRESERVE successful
 * delivery, so those use the asserted lrig_settle() above and additionally
 * pin their declared end state.
 */
static void lrig_settle_quiet(lrig_t *r)
{
    for (int i = 0; i < LRIG_SETTLE_BOUND; i++) {
        (void)moq_simpair_run_until_quiescent(r->sp, 32, NULL);
        if (moq_simpair_delayed_count(r->sp) == 0) break;
        uint64_t d = moq_simpair_next_deadline_us(r->sp);
        if (d == UINT64_MAX) break;
        if (d < moq_simpair_now_us(r->sp)) d = moq_simpair_now_us(r->sp);
        (void)moq_simpair_advance_to(r->sp, d);
    }
    (void)moq_simpair_run_until_quiescent(r->sp, 32, NULL);
}

/*
 * Row selection is DELIBERATELY BLIND to the expected subtype: it takes the
 * FIRST INPUT record after the row marker and then asserts its input_kind.
 * Selecting by the expected kind would let the checker skip past a wrong
 * record and pick a legitimate later side effect (a STOP can cause a RESET),
 * which is exactly what made the kind oracle self-selecting.
 */
static const cap_t *first_input(const cap_log_t *l, size_t from)
{
    for (size_t i = from; i < l->n; i++)
        if (l->recs[i].kind == MOQ_SIM_TRACE_INPUT &&
            l->recs[i].input_kind != MOQ_SIM_INPUT_TICK)
            return &l->recs[i];
    return NULL;
}

/*
 * TICK is the one declared exception, and it is not a stream-carrying kind:
 * the delayed lane advances virtual time to a delayed item's due instant, and
 * the pump traces the tick it performs before delivering it. Excluding TICK
 * therefore cannot let one stream event stand in for another -- which is the
 * property this selection exists to have. Everything else in the row's slice
 * is accounted for by kind below.
 */
static size_t count_input_kind(const cap_log_t *l, size_t from,
                               moq_sim_trace_input_kind_t k)
{
    size_t n = 0;
    for (size_t i = from; i < l->n; i++)
        if (l->recs[i].kind == MOQ_SIM_TRACE_INPUT &&
            l->recs[i].input_kind == k)
            n++;
    return n;
}

/* Every INPUT in the slice must be a tick, the triggered kind, or the row's
 * own DECLARED side effect -- nothing else, and nothing undeclared. */
static bool inputs_accounted(const cap_log_t *l, size_t from,
                             moq_sim_trace_input_kind_t trig,
                             moq_sim_trace_input_kind_t side)
{
    for (size_t i = from; i < l->n; i++) {
        if (l->recs[i].kind != MOQ_SIM_TRACE_INPUT) continue;
        moq_sim_trace_input_kind_t k = l->recs[i].input_kind;
        if (k == MOQ_SIM_INPUT_TICK || k == trig) continue;
        if (side != 0 && k == side) continue;
        return false;
    }
    return true;
}

/*
 * One required INPUT row. Every legacy field is declared, not only the three
 * the first revision checked: a producer that traced the right-length WRONG
 * bytes, reversed the perspectives, or newly populated another frozen field
 * would otherwise pass while the report claimed the legacy image was exact.
 */
typedef struct {
    const char                *cls;
    moq_sim_trace_input_kind_t kind;
    uint64_t                   want_ref;      /* from the LIVE map snapshot */
    uint64_t                   other_ref;     /* the far end of that mapping */
    moq_perspective_t          want_from;
    moq_perspective_t          want_to;
    const uint8_t             *want_bytes;    /* NULL for terminal rows */
    size_t                     want_bytes_len;
    uint64_t                   want_error_code;
    bool                       want_fin;
    moq_result_t               want_result;
    /* a declared, protocol-required side-effect INPUT -- never eligible to
     * stand in for the triggered row */
    moq_sim_trace_input_kind_t side_kind;
    size_t                     side_count;
} input_row_t;

static void check_input_row(const cap_log_t *log, size_t from,
                            const input_row_t *w)
{
    const cap_t *g = first_input(log, from);

    /* A missing INPUT is a failure -- no fallback to an ACTION record. */
    if (!g) { checkf(w->cls, "record_present", false); return; }

    /* The triggered subtype must be what the FIRST input record carries, and
     * it must occur exactly once in this row's slice. A later side-effect
     * INPUT of another kind is permitted; a second one of this kind is not. */
    checkf(w->cls, "prefix.input_kind", g->input_kind == w->kind);
    checkf(w->cls, "prefix.input_kind_once",
           count_input_kind(log, from, w->kind) == 1);
    /* declared side effects, counted exactly; anything else is unaccounted */
    if (w->side_kind != 0)
        checkf(w->cls, "prefix.side_effect_count",
               count_input_kind(log, from, w->side_kind) == w->side_count);
    checkf(w->cls, "prefix.no_undeclared_input",
           inputs_accounted(log, from, w->kind, w->side_kind));

    /* the mapping oracle: exact equality with the value read from the live map */
    checkf(w->cls, "detail.mapped_ref", g->stream_ref == w->want_ref);
    /* secondary only: today's allocator gives the two ends distinct refs */
    if (w->other_ref != 0 && w->want_ref != 0)
        checkf(w->cls, "detail.ref_differs_from_sender",
               g->stream_ref != w->other_ref);

    checkf(w->cls, "detail.error_code", g->error_code == w->want_error_code);
    checkf(w->cls, "detail.fin",        g->fin == w->want_fin);
    /* the appended detail span's exact EMPTY SHAPE, not its length alone */
    checkf(w->cls, "detail.bytes_len_zero", g->detail_len == 0);
    checkf(w->cls, "detail.bytes_null",     g->detail_null);

    /* the complete legacy image */
    checkf(w->cls, "prefix.from",   g->from == w->want_from);
    checkf(w->cls, "prefix.to",     g->to == w->want_to);
    checkf(w->cls, "prefix.bytes_len",  g->bytes_len == w->want_bytes_len);
    if (w->want_bytes_len > 0) {
        checkf(w->cls, "prefix.bytes_nonnull", !g->bytes_null);
        checkf(w->cls, "prefix.bytes_content",
               g->bytes_len == w->want_bytes_len && !g->bytes_null &&
               memcmp(g->bytes, w->want_bytes, w->want_bytes_len) == 0);
    } else {
        /* canonical empty legacy bytes: NULL pointer AND zero length */
        checkf(w->cls, "prefix.bytes_null", g->bytes_null);
    }
    checkf(w->cls, "prefix.action_kind_zero", g->action_kind == 0);
    checkf(w->cls, "prefix.code_zero",  g->code == 0);
    checkf(w->cls, "prefix.count_zero", g->count == 0);
    checkf(w->cls, "prefix.result",     g->result == w->want_result);
    checkf(w->cls, "prefix.struct_size",
           g->struct_size == sizeof(moq_sim_trace_record_t));
}


/* Establish one real data stream and one real request bidi, so both maps hold
 * live entries this test can read expected refs out of. */
typedef struct {
    moq_subscription_t csub, ssub;
    moq_subgroup_handle_t sg;
    bool ok;
} lflow_t;

static lflow_t lrig_flow_ex(lrig_t *r, const char *cls, bool quiet)
{
    lflow_t f;
    moq_event_t ev;
    moq_bytes_t nsp[] = { MOQ_BYTES_LITERAL("ns") };
    moq_namespace_t ns = { nsp, 1 };
    moq_subscribe_cfg_t sc;
    moq_subgroup_cfg_t gc;
    moq_rcbuf_t *p = NULL;

    memset(&f, 0, sizeof(f));
    f.ssub = MOQ_SUBSCRIPTION_INVALID;

    moq_session_t *c = moq_simpair_client(r->sp);
    moq_session_t *sv = moq_simpair_server(r->sp);

    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = ns;
    sc.track_name = MOQ_BYTES_LITERAL("t");
    if (moq_session_subscribe(c, &sc, 1000, &f.csub) != MOQ_OK) return f;
    if (quiet) lrig_settle_quiet(r); else lrig_settle(r, cls, "flow.subscribe");
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            f.ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    if (moq_session_accept_subscribe(sv, f.ssub, &acc, 1000) != MOQ_OK) return f;
    if (quiet) lrig_settle_quiet(r); else lrig_settle(r, cls, "flow.accept");
    drain_events(c);

    moq_subgroup_cfg_init(&gc);
    gc.group_id = 0; gc.subgroup_id = 0; gc.publisher_priority = 128;
    if (moq_session_open_subgroup(sv, f.ssub, &gc, 1000, &f.sg) != MOQ_OK)
        return f;
    if (quiet) lrig_settle_quiet(r); else lrig_settle(r, cls, "flow.open_subgroup");
    if (moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"hi", 2, &p)
        != MOQ_OK) return f;
    if (moq_session_write_object(sv, f.sg, 0, p, 1000) != MOQ_OK) {
        moq_rcbuf_decref(p); return f;
    }
    moq_rcbuf_decref(p);
    if (quiet) lrig_settle_quiet(r); else lrig_settle(r, cls, "flow.write_object");
    drain_events(c);
    f.ok = true;
    return f;
}

/* Asserted flow: every settlement must converge and return MOQ_OK. */
static lflow_t lrig_flow(lrig_t *r, const char *cls)
{
    return lrig_flow_ex(r, cls, false);
}

/* Flow for the FAULT rigs ONLY: injected faults may legitimately break a
 * delivery, so its settlements are unasserted (see lrig_settle_quiet). */
static lflow_t lrig_flow_quiet(lrig_t *r)
{
    return lrig_flow_ex(r, "flow.quiet", true);
}

/*
 * The map authority. Each arm requires EXACTLY ONE eligible mapping, captures
 * its slot, generation and both endpoint refs, and proves the production
 * lookups land on that slot in both directions. The snapshot -- not a fresh
 * scan -- is what the row carries, so a later entry that happens to match
 * cannot be substituted for the one the row established.
 */
typedef struct {
    bool     ok;
    int      slot;
    uint64_t generation;
    uint64_t near_ref;   /* data: sender_ref   · bidi: opener_ref    */
    uint64_t far_ref;    /* data: receiver_ref · bidi: responder_ref */
} mapsnap_t;

/* Data streams have one production lookup, keyed by (sender_ref, sender). The
 * receiver direction has no production helper, so the reverse resolution is a
 * test-side uniqueness scan and is labelled as such. */
static int data_find_by_receiver(moq_simpair_t *sp, uint64_t rref, int *n_out)
{
    int found = -1, n = 0;
    for (int i = 0; i < MOQ_SIM_MAX_DATA_STREAMS; i++)
        if (sp->stream_map[i].active && sp->stream_map[i].receiver_ref == rref) {
            if (found < 0) found = i;
            n++;
        }
    *n_out = n;
    return found;
}

static void map_check_data(lrig_t *r, const mapsnap_t *m, const char *cls,
                           const char *when)
{
    char f[96];
    moq_sim_stream_map_t *e = &r->sp->stream_map[m->slot];
    int n = 0, byrecv;

    snprintf(f, sizeof(f), "%s.active", when);
    checkf(cls, f, e->active);
    snprintf(f, sizeof(f), "%s.generation", when);
    checkf(cls, f, e->generation == m->generation);
    snprintf(f, sizeof(f), "%s.sender_ref", when);
    checkf(cls, f, e->sender_ref == m->near_ref);
    snprintf(f, sizeof(f), "%s.receiver_ref", when);
    checkf(cls, f, e->receiver_ref == m->far_ref);
    snprintf(f, sizeof(f), "%s.lookup_by_sender", when);
    checkf(cls, f, sim_stream_map_find(r->sp, m->near_ref,
                                       MOQ_PERSPECTIVE_SERVER) == m->slot);
    byrecv = data_find_by_receiver(r->sp, m->far_ref, &n);
    snprintf(f, sizeof(f), "%s.scan_by_receiver", when);
    checkf(cls, f, n == 1 && byrecv == m->slot);
}

static mapsnap_t snap_data_map(lrig_t *r, const char *cls)
{
    mapsnap_t m;
    int n = 0, first = -1;

    memset(&m, 0, sizeof(m));
    for (int i = 0; i < MOQ_SIM_MAX_DATA_STREAMS; i++)
        if (r->sp->stream_map[i].active &&
            r->sp->stream_map[i].sender == MOQ_PERSPECTIVE_SERVER) {
            if (first < 0) first = i;
            n++;
        }
    checkf(cls, "map.exactly_one", n == 1);
    if (n != 1) return m;

    m.slot       = first;
    m.generation = r->sp->stream_map[first].generation;
    m.near_ref   = r->sp->stream_map[first].sender_ref;
    m.far_ref    = r->sp->stream_map[first].receiver_ref;
    m.ok         = true;
    map_check_data(r, &m, cls, "map.arm");
    return m;
}

static void map_check_bidi(lrig_t *r, const mapsnap_t *m, const char *cls,
                           const char *when)
{
    char f[96];
    moq_sim_bidi_map_t *e = &r->sp->bidi_map[m->slot];

    snprintf(f, sizeof(f), "%s.active", when);
    checkf(cls, f, e->active);
    snprintf(f, sizeof(f), "%s.generation", when);
    checkf(cls, f, e->generation == m->generation);
    snprintf(f, sizeof(f), "%s.opener_ref", when);
    checkf(cls, f, e->opener_ref == m->near_ref);
    snprintf(f, sizeof(f), "%s.responder_ref", when);
    checkf(cls, f, e->responder_ref == m->far_ref);
    snprintf(f, sizeof(f), "%s.lookup_by_opener", when);
    checkf(cls, f, sim_bidi_find_by_opener(r->sp, m->near_ref,
                                           MOQ_PERSPECTIVE_CLIENT) == m->slot);
    snprintf(f, sizeof(f), "%s.lookup_by_responder", when);
    checkf(cls, f, sim_bidi_find_by_responder(r->sp, m->far_ref,
                                              MOQ_PERSPECTIVE_CLIENT) == m->slot);
}

static mapsnap_t snap_bidi_map(lrig_t *r, const char *cls)
{
    mapsnap_t m;
    int n = 0, first = -1;

    memset(&m, 0, sizeof(m));
    for (int i = 0; i < MOQ_SIM_MAX_BIDI_STREAMS; i++)
        if (r->sp->bidi_map[i].active &&
            r->sp->bidi_map[i].opener == MOQ_PERSPECTIVE_CLIENT) {
            if (first < 0) first = i;
            n++;
        }
    checkf(cls, "map.exactly_one", n == 1);
    if (n != 1) return m;

    m.slot       = first;
    m.generation = r->sp->bidi_map[first].generation;
    m.near_ref   = r->sp->bidi_map[first].opener_ref;
    m.far_ref    = r->sp->bidi_map[first].responder_ref;
    m.ok         = true;
    map_check_bidi(r, &m, cls, "map.arm");
    return m;
}

#define IN_CODE_DATA_RESET 0x41ULL
#define IN_CODE_DATA_STOP  0x42ULL
#define IN_CODE_BIDI_RESET 0x43ULL
#define IN_CODE_BIDI_STOP  0x44ULL

typedef enum {
    ROW_DATA_BYTES,
    ROW_BIDI_BYTES,
    ROW_DATA_STOP,
    ROW_DATA_RESET,
    ROW_BIDI_STOP,
    ROW_BIDI_RESET,
} row_kind_t;

/*
 * EVERY row runs on its OWN freshly established pair, including each FIN
 * value. Sharing a pair made later rows depend on earlier ones: a STOP queues
 * a RESET whose delivery retires the data map, so the RESET row that followed
 * it was operating on a stale slot and only appeared to pass. Each row now
 * starts from the same declared live-map precondition, and that precondition
 * is reasserted immediately before delivery.
 */
static void run_input_row(bool delay_all, const char *lane,
                          row_kind_t rk, bool want_fin)
{
    static const uint8_t DCHUNK[] = { 0xC1, 0xC2, 0xC3 };
    static const uint8_t BCHUNK[] = { 0xB1, 0xB2 };
    lrig_t r;
    char cls[160];
    const char *name;
    bool is_data;

    switch (rk) {
    case ROW_DATA_BYTES: name = want_fin ? "data_bytes.fin1"
                                         : "data_bytes.fin0"; is_data = true;  break;
    case ROW_BIDI_BYTES: name = want_fin ? "bidi_bytes.fin1"
                                         : "bidi_bytes.fin0"; is_data = false; break;
    case ROW_DATA_STOP:  name = "data_stop";  is_data = true;  break;
    case ROW_DATA_RESET: name = "data_reset"; is_data = true;  break;
    case ROW_BIDI_STOP:  name = "bidi_stop";  is_data = false; break;
    default:             name = "bidi_reset"; is_data = false; break;
    }
    snprintf(cls, sizeof(cls), "input.%s.%s", lane, name);

    if (lrig_up(&r, delay_all) != 0) { checkf(cls, "rig", false); return; }
    lflow_t f = lrig_flow(&r, cls);
    checkf(cls, "flow", f.ok);
    if (!f.ok) { moq_simpair_destroy(r.sp); return; }

    mapsnap_t m = is_data ? snap_data_map(&r, cls) : snap_bidi_map(&r, cls);
    if (!m.ok) { moq_simpair_destroy(r.sp); return; }

    /* the declared precondition, reasserted immediately before delivery */
    if (is_data) map_check_data(&r, &m, cls, "map.pre");
    else         map_check_bidi(&r, &m, cls, "map.pre");

    moq_session_t *cl = moq_simpair_client(r.sp);
    moq_session_t *sv = moq_simpair_server(r.sp);
    size_t mark = r.log.n;
    input_row_t w;
    moq_session_t *drain_at = NULL;
    /* The direct call is the row's ARM: in the immediate lane it delivers, in
     * the delayed lane it enqueues. Either way a refusal means the row never
     * happened, so its result is asserted rather than discarded. */
    moq_result_t drc = MOQ_ERR_INTERNAL;

    memset(&w, 0, sizeof(w));
    w.cls = cls;
    w.want_result = MOQ_OK;

    switch (rk) {
    case ROW_DATA_BYTES:
        drc = deliver_or_delay_data_chunk(
            r.sp, cl, moq_stream_ref_from_u64(m.far_ref),
            DCHUNK, sizeof(DCHUNK), want_fin, m.slot,
            MOQ_PERSPECTIVE_SERVER, MOQ_PERSPECTIVE_CLIENT);
        drain_at = cl;
        w.kind = MOQ_SIM_INPUT_DATA_BYTES;
        w.want_ref = m.far_ref; w.other_ref = m.near_ref;
        w.want_from = MOQ_PERSPECTIVE_SERVER; w.want_to = MOQ_PERSPECTIVE_CLIENT;
        w.want_bytes = DCHUNK; w.want_bytes_len = sizeof(DCHUNK);
        w.want_fin = want_fin;
        break;
    case ROW_BIDI_BYTES:
        drc = deliver_or_delay_bidi_chunk(
            r.sp, sv, moq_stream_ref_from_u64(m.far_ref),
            BCHUNK, sizeof(BCHUNK), want_fin, m.slot,
            MOQ_PERSPECTIVE_CLIENT, MOQ_PERSPECTIVE_SERVER);
        drain_at = sv;
        w.kind = MOQ_SIM_INPUT_BIDI_BYTES;
        w.want_ref = m.far_ref; w.other_ref = m.near_ref;
        w.want_from = MOQ_PERSPECTIVE_CLIENT; w.want_to = MOQ_PERSPECTIVE_SERVER;
        w.want_bytes = BCHUNK; w.want_bytes_len = sizeof(BCHUNK);
        w.want_fin = want_fin;
        break;
    case ROW_DATA_STOP:
        /* STOP travels toward the SENDER: its ref is the sender-side ref. */
        drc = deliver_or_delay_data_stop(
            r.sp, sv, moq_stream_ref_from_u64(m.near_ref),
            IN_CODE_DATA_STOP, m.slot,
            MOQ_PERSPECTIVE_CLIENT, MOQ_PERSPECTIVE_SERVER);
        drain_at = sv;
        w.kind = MOQ_SIM_INPUT_DATA_STOP;
        w.want_ref = m.near_ref; w.other_ref = m.far_ref;
        w.want_from = MOQ_PERSPECTIVE_CLIENT; w.want_to = MOQ_PERSPECTIVE_SERVER;
        w.want_error_code = IN_CODE_DATA_STOP;
        /* the session answers a STOP on its own send half with a RESET, which
         * SimPair then delivers back: declared here, and by being declared it
         * cannot be selected as the triggered record. */
        w.side_kind = MOQ_SIM_INPUT_DATA_RESET; w.side_count = 1;
        break;
    case ROW_DATA_RESET:
        drc = deliver_or_delay_data_reset(
            r.sp, cl, moq_stream_ref_from_u64(m.far_ref),
            IN_CODE_DATA_RESET, m.slot,
            MOQ_PERSPECTIVE_SERVER, MOQ_PERSPECTIVE_CLIENT);
        drain_at = cl;
        w.kind = MOQ_SIM_INPUT_DATA_RESET;
        w.want_ref = m.far_ref; w.other_ref = m.near_ref;
        w.want_from = MOQ_PERSPECTIVE_SERVER; w.want_to = MOQ_PERSPECTIVE_CLIENT;
        w.want_error_code = IN_CODE_DATA_RESET;
        break;
    case ROW_BIDI_STOP:
        drc = deliver_or_delay_bidi_stop(
            r.sp, sv, moq_stream_ref_from_u64(m.far_ref),
            IN_CODE_BIDI_STOP, m.slot,
            MOQ_PERSPECTIVE_CLIENT, MOQ_PERSPECTIVE_SERVER);
        drain_at = sv;
        w.kind = MOQ_SIM_INPUT_BIDI_STOP;
        w.want_ref = m.far_ref; w.other_ref = m.near_ref;
        w.want_from = MOQ_PERSPECTIVE_CLIENT; w.want_to = MOQ_PERSPECTIVE_SERVER;
        w.want_error_code = IN_CODE_BIDI_STOP;
        break;
    default:
        drc = deliver_or_delay_bidi_reset(
            r.sp, sv, moq_stream_ref_from_u64(m.far_ref),
            IN_CODE_BIDI_RESET, m.slot,
            MOQ_PERSPECTIVE_CLIENT, MOQ_PERSPECTIVE_SERVER);
        drain_at = sv;
        w.kind = MOQ_SIM_INPUT_BIDI_RESET;
        w.want_ref = m.far_ref; w.other_ref = m.near_ref;
        w.want_from = MOQ_PERSPECTIVE_CLIENT; w.want_to = MOQ_PERSPECTIVE_SERVER;
        w.want_error_code = IN_CODE_BIDI_RESET;
        break;
    }

    checkf(cls, "arm.delivery_result", drc == MOQ_OK);
    lrig_settle(&r, cls, "row");
    drain_events(drain_at);
    check_input_row(&r.log, mark, &w);
    checkf(cls, "no_overflow",     !r.log.overflow);
    checkf(cls, "comparable",      !r.log.incomparable);
    moq_simpair_destroy(r.sp);
}

static void t_input_matrix(bool delay_all)
{
    int before = failures;
    const char *lane = delay_all ? "delayed" : "immediate";

    printf("INPUT-MATRIX (%s):\n", lane);
    run_input_row(delay_all, lane, ROW_DATA_BYTES, false);
    run_input_row(delay_all, lane, ROW_DATA_BYTES, true);
    run_input_row(delay_all, lane, ROW_BIDI_BYTES, false);
    run_input_row(delay_all, lane, ROW_BIDI_BYTES, true);
    run_input_row(delay_all, lane, ROW_DATA_STOP,  false);
    run_input_row(delay_all, lane, ROW_DATA_RESET, false);
    run_input_row(delay_all, lane, ROW_BIDI_STOP,  false);
    run_input_row(delay_all, lane, ROW_BIDI_RESET, false);
    if (failures == before) printf("PASS: input_matrix_%s\n", lane);
}

/* ===================================================================== *
 * 5. Frozen-v0 prefix: hard numeric hash controls
 *
 * The four scenarios below were captured from the pre-append producer. Their
 * hashes are written here as literals, so a producer edit that newly
 * populates any legacy field changes a number this file already declares --
 * the twin-run suites would move with it, and this fails first and by name.
 * ===================================================================== */

typedef struct {
    const char *name;
    moq_version_t version;
    bool        do_close;
    size_t      expect_records;
    uint64_t    expect_hash;
} v0_case_t;

static void run_v0_case(const v0_case_t *k)
{
    cap_log_t log;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    moq_simpair_t *sp = NULL;
    char cls[96];

    cap_reset(&log);
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x51ULL;                    /* the captured seed */
    cfg.version = k->version;
    cfg.trace_fn = cap_fn;
    cfg.trace_ctx = &log;

    snprintf(cls, sizeof(cls), "v0.%s", k->name);
    if (moq_simpair_create(&cfg, &sp) != MOQ_OK || !sp) {
        checkf(cls, "create", false);
        return;
    }
    moq_simpair_start(sp);
    (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
    if (k->do_close) {
        static const char reason[] = "preflight reason";
        (void)moq_session_close(moq_simpair_client(sp), 0x99, reason,
                                moq_simpair_now_us(sp));
        (void)moq_simpair_run_until_quiescent(sp, 64, NULL);
    }

    checkf(cls, "record_count", log.n == k->expect_records);
    checkf(cls, "prefix_hash",  log.hash == k->expect_hash);
    if (log.hash != k->expect_hash)
        fprintf(stderr, "      %s: hash 0x%016llx expected 0x%016llx\n",
                k->name, (unsigned long long)log.hash,
                (unsigned long long)k->expect_hash);
    moq_simpair_destroy(sp);
}

static void t_v0_prefix_frozen(void)
{
    int before = failures;
    static const v0_case_t CASES[] = {
        { "d16_setup",       MOQ_VERSION_DRAFT_16, false, 6, 0xc4194c3225ccd16dULL },
        { "d18_setup",       MOQ_VERSION_DRAFT_18, false, 7, 0x13a301c376060f0dULL },
        { "d16_setup_close", MOQ_VERSION_DRAFT_16, true,  8, 0xb8351ef81eeba2b5ULL },
        { "d18_setup_close", MOQ_VERSION_DRAFT_18, true,  9, 0x14a3c083cae1b755ULL },
    };

    printf("V0-PREFIX-FROZEN:\n");
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++)
        run_v0_case(&CASES[i]);

    if (failures == before) printf("PASS: v0_prefix_frozen\n");
}

/* The same freeze over a data-bearing run, where SEND_DATA, request bidis and
 * a reset all appear -- the record classes the append touches most. */
static void t_v0_prefix_frozen_data(void)
{
    int before = failures;
    cap_log_t log;
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    moq_simpair_t *sp = NULL;

    printf("V0-PREFIX-FROZEN-DATA:\n");
    cap_reset(&log);
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x77ULL;                    /* the captured seed */
    cfg.version = MOQ_VERSION_DRAFT_18;
    cfg.trace_fn = cap_fn;
    cfg.trace_ctx = &log;
    if (moq_simpair_create(&cfg, &sp) != MOQ_OK || !sp) {
        checkf("v0.data", "create", false);
        return;
    }
    moq_simpair_start(sp);
    (void)moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_session_t *c = moq_simpair_client(sp), *sv = moq_simpair_server(sp);
    drain_events(c); drain_events(sv);

    moq_bytes_t nsp[] = { MOQ_BYTES_LITERAL("ns") };
    moq_namespace_t ns = { nsp, 1 };
    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    sc.track_namespace = ns;
    sc.track_name = MOQ_BYTES_LITERAL("t");
    moq_subscription_t csub;
    checkf("v0.data", "subscribe",
           moq_session_subscribe(c, &sc, 1000, &csub) == MOQ_OK);
    (void)moq_simpair_run_until_quiescent(sp, 16, NULL);

    moq_subscription_t ssub = MOQ_SUBSCRIPTION_INVALID;
    moq_event_t ev;
    while (moq_session_poll_events(sv, &ev, 1) > 0) {
        if (ev.kind == MOQ_EVENT_SUBSCRIBE_REQUEST)
            ssub = ev.u.subscribe_request.sub;
        moq_event_cleanup(&ev);
    }
    moq_accept_subscribe_cfg_t acc;
    moq_accept_subscribe_cfg_init(&acc);
    checkf("v0.data", "accept",
           moq_session_accept_subscribe(sv, ssub, &acc, 1000) == MOQ_OK);
    (void)moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain_events(c);

    moq_subgroup_cfg_t gc;
    moq_subgroup_cfg_init(&gc);
    gc.group_id = 0; gc.subgroup_id = 0; gc.publisher_priority = 128;
    moq_subgroup_handle_t sg;
    checkf("v0.data", "open_subgroup",
           moq_session_open_subgroup(sv, ssub, &gc, 1000, &sg) == MOQ_OK);
    (void)moq_simpair_run_until_quiescent(sp, 16, NULL);
    moq_rcbuf_t *p = NULL;
    moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"hello", 5, &p);
    (void)moq_session_write_object(sv, sg, 0, p, 1000);
    if (p) moq_rcbuf_decref(p);
    (void)moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain_events(c);

    (void)moq_session_reset_subgroup(sv, sg, 0x2a, 1000);
    (void)moq_simpair_run_until_quiescent(sp, 16, NULL);
    drain_events(c); drain_events(sv);

    checkf("v0.data", "record_count", log.n == 23);
    checkf("v0.data", "prefix_hash", log.hash == 0x19828170f016fd58ULL);
    if (log.hash != 0x19828170f016fd58ULL)
        fprintf(stderr, "      d18_data: hash 0x%016llx expected 0x%016llx\n",
                (unsigned long long)log.hash,
                (unsigned long long)0x19828170f016fd58ULL);

    moq_simpair_destroy(sp);
    if (failures == before) printf("PASS: v0_prefix_frozen_data\n");
}

/* ===================================================================== *
 * 6. Zero-detail records: every kind the contract declares to carry NO
 *    appended detail, driven through its REAL producer.
 *
 * Without this the four zero-detail INPUT kinds and every non-INPUT trace
 * kind were unconstrained: a call site could pass a nonzero ref, code or FIN
 * and the whole suite stayed green. Each row below pins the producer's
 * identity through its own legacy fields, so an unrelated zeroed record
 * cannot stand in for the expected one, and requires an exact count so a
 * missing producer call fails rather than passing vacuously.
 * ===================================================================== */

typedef struct {
    const char                *cls;
    moq_sim_trace_kind_t       kind;
    /* legacy identity pins -- which of these apply is per producer. Every row
     * below uses DESIGNATED initializers: a positional list cannot be audited
     * at a glance, and inserting a member would silently reinterpret pins. */
    bool                       pin_input_kind;
    moq_sim_trace_input_kind_t input_kind;
    bool                       pin_action_kind;
    moq_action_kind_t          action_kind;
    bool                       pin_code;
    uint64_t                   code;
    bool                       pin_from;
    moq_perspective_t          from;
    bool                       pin_to;
    moq_perspective_t          to;
    /* ASSERTED (never selectors): applied to every matching record */
    bool                       want_result_set;
    moq_result_t               want_result;
    bool                       want_bytes_nonempty;   /* non-NULL and len > 0 */
    const uint8_t             *want_bytes;            /* exact legacy content */
    size_t                     want_bytes_len;
    bool                       exact;       /* count must equal want_count  */
    size_t                     want_count;  /* else it is a minimum         */
} zero_row_t;

/* The trace kinds this zero-detail matrix is FOR. ACTION and FAULT_DROP are
 * deliberately excluded: they have their own nonzero-detail contract and are
 * covered by the action table, so declaring one here would be a policy error,
 * not a zero-detail row. */
static bool zero_row_kind_allowed(moq_sim_trace_kind_t k)
{
    switch (k) {
    case MOQ_SIM_TRACE_INPUT:
    case MOQ_SIM_TRACE_QUIESCENT:
    case MOQ_SIM_TRACE_FAULT_MUTATE:
    case MOQ_SIM_TRACE_FAULT_REORDER:
    case MOQ_SIM_TRACE_FAULT_INJECT:
    case MOQ_SIM_TRACE_FAULT_TRUNCATE:
    case MOQ_SIM_TRACE_DELAY_ENQUEUE:
    case MOQ_SIM_TRACE_DELAY_STALE:
        return true;
    default:
        return false;    /* ACTION, FAULT_DROP, zero, and unknown values */
    }
}

static bool zero_row_input_kind_valid(moq_sim_trace_input_kind_t k)
{
    switch (k) {
    case MOQ_SIM_INPUT_START:
    case MOQ_SIM_INPUT_CONTROL_BYTES:
    case MOQ_SIM_INPUT_TICK:
    case MOQ_SIM_INPUT_DATA_BYTES:
    case MOQ_SIM_INPUT_DATA_RESET:
    case MOQ_SIM_INPUT_DATA_STOP:
    case MOQ_SIM_INPUT_BIDI_BYTES:
    case MOQ_SIM_INPUT_BIDI_RESET:
    case MOQ_SIM_INPUT_DATAGRAM:
    case MOQ_SIM_INPUT_BIDI_STOP:
        return true;
    default:
        return false;
    }
}

static bool zero_row_action_kind_valid(moq_action_kind_t k)
{
    switch (k) {
    case MOQ_ACTION_SEND_CONTROL:
    case MOQ_ACTION_CLOSE_SESSION:
    case MOQ_ACTION_SEND_DATA:
    case MOQ_ACTION_RESET_DATA:
    case MOQ_ACTION_STOP_DATA:
    case MOQ_ACTION_OPEN_BIDI_STREAM:
    case MOQ_ACTION_SEND_BIDI_STREAM:
    case MOQ_ACTION_CLOSE_BIDI_STREAM:
    case MOQ_ACTION_SEND_DATAGRAM:
    case MOQ_ACTION_OPEN_UNI_CONTROL:
    case MOQ_ACTION_SEND_UNI_CONTROL:
    case MOQ_ACTION_RESET_BIDI_STREAM:
    case MOQ_ACTION_STOP_BIDI_STREAM:
    case MOQ_ACTION_ABORT_BIDI_STREAM:
        return true;
    default:
        return false;
    }
}

/*
 * A row must name itself, name a kind this matrix is allowed to declare,
 * require at least one record, pin only real subtypes, and describe a byte
 * span that is BOUNDS-SAFE: pointer and length present or absent TOGETHER
 * (a length without a pointer silently disables the exact-byte check), and
 * never longer than the capture buffer the comparison reads from.
 */
static bool zero_row_valid(const zero_row_t *w)
{
    if (!w->cls || !*w->cls) return false;
    if (!zero_row_kind_allowed(w->kind)) return false;
    if (w->want_count == 0) return false;
    if ((w->want_bytes != NULL) != (w->want_bytes_len != 0)) return false;
    if (w->want_bytes_len > CAP_BYTES) return false;
    if (w->pin_input_kind && !zero_row_input_kind_valid(w->input_kind))
        return false;
    if (w->pin_action_kind && !zero_row_action_kind_valid(w->action_kind))
        return false;
    return true;
}

static bool zero_row_matches(const cap_t *c, const zero_row_t *w)
{
    if (c->kind != w->kind) return false;
    if (w->pin_input_kind  && c->input_kind  != w->input_kind)  return false;
    if (w->pin_action_kind && c->action_kind != w->action_kind) return false;
    if (w->pin_code        && c->code        != w->code)        return false;
    if (w->pin_from        && c->from        != w->from)        return false;
    if (w->pin_to          && c->to          != w->to)          return false;
    return true;
}

/* Every matching record must carry the FULL record and the canonical zero
 * shape in all four appended fields -- including the empty detail span's
 * NULL pointer, not merely a zero length. */
static void check_zero_rows(const cap_log_t *log, size_t from,
                            const zero_row_t *rows, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        const zero_row_t *w = &rows[k];
        size_t seen = 0;
        bool size_ok = true, ref_ok = true, code_ok = true;
        bool fin_ok = true, span_ok = true, result_ok = true;
        bool bytes_ok = true;

        /* A partially declared row must not pass vacuously. */
        if (!zero_row_valid(w)) {
            checkf(w->cls ? w->cls : "zero_row.unnamed", "descriptor", false);
            continue;
        }

        for (size_t i = from; i < log->n; i++) {
            const cap_t *c = &log->recs[i];
            if (!zero_row_matches(c, w)) continue;
            seen++;
            if (c->struct_size != sizeof(moq_sim_trace_record_t)) size_ok = false;
            if (c->stream_ref != 0)   ref_ok = false;
            if (c->error_code != 0)   code_ok = false;
            if (c->fin)               fin_ok = false;
            if (c->detail_len != 0 || !c->detail_null) span_ok = false;
            if (w->want_result_set && c->result != w->want_result)
                result_ok = false;
            if (w->want_bytes_nonempty && (c->bytes_null || c->bytes_len == 0))
                bytes_ok = false;
            if (w->want_bytes) {
                if (c->bytes_len != w->want_bytes_len || c->bytes_null ||
                    memcmp(c->bytes, w->want_bytes, w->want_bytes_len) != 0)
                    bytes_ok = false;
            }
        }
        if (w->exact) checkf(w->cls, "count_exact", seen == w->want_count);
        else          checkf(w->cls, "count_min",   seen >= w->want_count);
        if (seen == 0) continue;   /* the count check already named it */
        checkf(w->cls, "struct_size",       size_ok);
        checkf(w->cls, "stream_ref_zero",   ref_ok);
        checkf(w->cls, "error_code_zero",   code_ok);
        checkf(w->cls, "fin_false",         fin_ok);
        checkf(w->cls, "detail_empty_shape", span_ok);
        if (w->want_result_set) checkf(w->cls, "legacy_result", result_ok);
        if (w->want_bytes_nonempty || w->want_bytes)
            checkf(w->cls, "legacy_bytes", bytes_ok);
    }
}

/* A pair with an explicit fault configuration. Faults are enabled only AFTER
 * the handshake, exactly as the delayed INPUT lane does. */
static int zrig_up(lrig_t *r, const char *cls, moq_version_t ver,
                   uint32_t flags, uint32_t per_mille, bool arm_before_start)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;

    cap_reset(&r->log);
    cfg.alloc = moq_alloc_default();
    cfg.seed = 0x5eedu;
    cfg.version = ver;
    cfg.trace_fn = cap_fn;
    cfg.trace_ctx = &r->log;
    cfg.fault_per_mille = per_mille;
    cfg.fault_flags = flags;
    r->sp = NULL;
    if (moq_simpair_create(&cfg, &r->sp) != MOQ_OK || !r->sp) {
        checkf(cls, "rig.create", false);
        return -1;
    }
    /* Arming BEFORE start subjects the HANDSHAKE itself to the fault, which is
     * the only live route to control-stream coverage on a profile whose
     * post-handshake traffic no longer uses the control stream. */
    if (flags && per_mille && arm_before_start) moq_simpair_enable_faults(r->sp);
    /* No caller has a legitimate reason for start itself to fail, and the
     * first pump runs before any fault-injection rig arms its faults -- so
     * BOTH are asserted exactly, never discarded. */
    checkf(cls, "rig.start", moq_simpair_start(r->sp) == MOQ_OK);
    checkf(cls, "rig.first_pump",
           moq_simpair_run_until_quiescent(r->sp, 32, NULL) == MOQ_OK);
    drain_events(moq_simpair_client(r->sp));
    drain_events(moq_simpair_server(r->sp));
    if (flags && per_mille && !arm_before_start) moq_simpair_enable_faults(r->sp);
    return 0;
}

/* START, TICK, CONTROL_BYTES (immediate) and DATAGRAM, on live paths. */
static void t_zero_detail_input(void)
{
    int before = failures;
    lrig_t r;

    printf("ZERO-DETAIL-INPUT:\n");
    if (lrig_up(&r, false) != 0) { CHECKN("zin.rig", false); return; }

    /* START x2 and the whole d18 handshake's CONTROL_BYTES are already in the
     * log: lrig_up() started the pair and pumped it to quiescence. */
    static const zero_row_t START_ROWS[] = {
        { .cls = "zin.start.c2s", .kind = MOQ_SIM_TRACE_INPUT,
          .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_START,
          .pin_from = true, .from = MOQ_PERSPECTIVE_CLIENT,
          .pin_to = true, .to = MOQ_PERSPECTIVE_SERVER,
          .want_result_set = true, .want_result = MOQ_OK,
          .exact = true, .want_count = 1 },
        { .cls = "zin.start.s2c", .kind = MOQ_SIM_TRACE_INPUT,
          .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_START,
          .pin_from = true, .from = MOQ_PERSPECTIVE_SERVER,
          .pin_to = true, .to = MOQ_PERSPECTIVE_CLIENT,
          .want_result_set = true, .want_result = MOQ_OK,
          .exact = true, .want_count = 1 },
        { .cls = "zin.control.handshake", .kind = MOQ_SIM_TRACE_INPUT,
          .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_CONTROL_BYTES,
          .want_result_set = true, .want_result = MOQ_OK,
          .want_bytes_nonempty = true, .want_count = 1 },
    };
    check_zero_rows(&r.log, 0, START_ROWS,
                    sizeof(START_ROWS) / sizeof(START_ROWS[0]));

    /* TICK: one virtual-time advance traces a tick for BOTH perspectives. */
    size_t mark = r.log.n;
    CHECKN("zin.tick.advance",
           moq_simpair_advance_to(r.sp, moq_simpair_now_us(r.sp) + 1000)
           == MOQ_OK);
    static const zero_row_t TICK_ROWS[] = {
        { .cls = "zin.tick.c2s", .kind = MOQ_SIM_TRACE_INPUT,
          .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_TICK,
          .pin_from = true, .from = MOQ_PERSPECTIVE_CLIENT,
          .pin_to = true, .to = MOQ_PERSPECTIVE_SERVER,
          .want_result_set = true, .want_result = MOQ_OK,
          .exact = true, .want_count = 1 },
        { .cls = "zin.tick.s2c", .kind = MOQ_SIM_TRACE_INPUT,
          .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_TICK,
          .pin_from = true, .from = MOQ_PERSPECTIVE_SERVER,
          .pin_to = true, .to = MOQ_PERSPECTIVE_CLIENT,
          .want_result_set = true, .want_result = MOQ_OK,
          .exact = true, .want_count = 1 },
    };
    check_zero_rows(&r.log, mark, TICK_ROWS, 2);

    /* DATAGRAM through the pump: a real publisher-side object datagram. */
    lflow_t f = lrig_flow(&r, "zin.flow");
    CHECKN("zin.flow.ok", f.ok);
    if (f.ok) {
        moq_rcbuf_t *p = NULL;
        CHECKN("zin.datagram.payload",
               moq_rcbuf_create(moq_alloc_default(),
                                (const uint8_t *)"dg", 2, &p) == MOQ_OK);
        if (p) {
            mark = r.log.n;
            CHECKN("zin.datagram.send",
                   moq_session_send_object_datagram(
                       moq_simpair_server(r.sp), f.ssub, 1, 0, 128, false,
                       p, NULL, 0, moq_simpair_now_us(r.sp)) == MOQ_OK);
            moq_rcbuf_decref(p);
            lrig_settle(&r, "zin.datagram", "row");
            drain_events(moq_simpair_client(r.sp));
            static const zero_row_t DG_ROWS[] = {
                { .cls = "zin.datagram", .kind = MOQ_SIM_TRACE_INPUT,
                  .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_DATAGRAM,
                  .pin_from = true, .from = MOQ_PERSPECTIVE_SERVER,
                  .pin_to = true, .to = MOQ_PERSPECTIVE_CLIENT,
                  .want_result_set = true, .want_result = MOQ_OK,
                  .want_bytes_nonempty = true,
                  .exact = true, .want_count = 1 },
            };
            check_zero_rows(&r.log, mark, DG_ROWS, 1);
        }
    }
    CHECKN("zin.no_overflow", !r.log.overflow);
    CHECKN("zin.comparable",  !r.log.incomparable);
    moq_simpair_destroy(r.sp);
    if (failures == before) printf("PASS: zero_detail_input\n");
}

/* CONTROL_BYTES delivered through the MATURED DELAY path, plus the two
 * scheduling record kinds. */
static void t_zero_detail_delay(void)
{
    int before = failures;

    printf("ZERO-DETAIL-DELAY:\n");

    /*
     * (a) CONTROL_BYTES delivered through the MATURED DELAY path, plus the
     * DELAY_ENQUEUE scheduling record that carries it. Draft-16 with the
     * delay fault armed BEFORE start: the whole handshake rides the control
     * stream, so every control delivery is enqueued and later matured.
     */
    {
        lrig_t r;
        if (zrig_up(&r, "zdla", MOQ_VERSION_DRAFT_16, MOQ_SIM_FAULT_DELAY, 1000, true)
            != 0) { CHECKN("zdla.rig", false); return; }
        /* Delay is NOT a hard fault: it must PRESERVE successful delivery, so
         * this arm uses the asserted settlement -- an error, a stalled queue
         * or a failed virtual-time advance fails the row by name. */
        lrig_settle(&r, "zdla", "handshake");
        /* and the declared end state: a delayed handshake still ESTABLISHES */
        CHECKN("zdla.client_established",
               moq_session_state(moq_simpair_client(r.sp))
               == MOQ_SESS_ESTABLISHED);
        CHECKN("zdla.server_established",
               moq_session_state(moq_simpair_server(r.sp))
               == MOQ_SESS_ESTABLISHED);
        CHECKN("zdla.drained", moq_simpair_delayed_count(r.sp) == 0);
        static const zero_row_t ROWS[] = {
            { .cls = "zdla.enqueue.control",
              .kind = MOQ_SIM_TRACE_DELAY_ENQUEUE,
              .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_CONTROL_BYTES,
              .want_count = 1 },
            /* the matured delivery must have SUCCEEDED and carried real bytes:
             * delay preserves successful delivery, it does not excuse one */
            { .cls = "zdla.control.matured", .kind = MOQ_SIM_TRACE_INPUT,
              .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_CONTROL_BYTES,
              .want_result_set = true, .want_result = MOQ_OK,
              .want_bytes_nonempty = true, .want_count = 1 },
            { .cls = "zdla.quiescent", .kind = MOQ_SIM_TRACE_QUIESCENT,
              .want_count = 1 },
        };
        check_zero_rows(&r.log, 0, ROWS, 3);
        CHECKN("zdla.no_overflow", !r.log.overflow);
        CHECKN("zdla.comparable",  !r.log.incomparable);
        moq_simpair_destroy(r.sp);
    }

    /*
     * (b) DELAY_STALE through its real producer: two delayed RESETs are
     * enqueued against the same live data slot and generation. The first
     * matures and retires the slot (generation++), so the second is stale
     * when its turn comes and takes trace_delay_stale().
     */
    {
        lrig_t r;
        if (zrig_up(&r, "zdlb", MOQ_VERSION_DRAFT_18, MOQ_SIM_FAULT_DELAY, 1000, false)
            != 0) { CHECKN("zdlb.rig", false); return; }
        lflow_t f = lrig_flow(&r, "zdlb.flow");
        CHECKN("zdlb.flow.ok", f.ok);
        if (!f.ok) { moq_simpair_destroy(r.sp); return; }

        mapsnap_t m = snap_data_map(&r, "zdlb.stale");
        if (m.ok) {
            size_t mark = r.log.n;
            moq_session_t *cl = moq_simpair_client(r.sp);
            CHECKN("zdlb.stale.enqueue1",
                   deliver_or_delay_data_reset(
                       r.sp, cl, moq_stream_ref_from_u64(m.far_ref), 0x51,
                       m.slot, MOQ_PERSPECTIVE_SERVER,
                       MOQ_PERSPECTIVE_CLIENT) == MOQ_OK);
            CHECKN("zdlb.stale.enqueue2",
                   deliver_or_delay_data_reset(
                       r.sp, cl, moq_stream_ref_from_u64(m.far_ref), 0x52,
                       m.slot, MOQ_PERSPECTIVE_SERVER,
                       MOQ_PERSPECTIVE_CLIENT) == MOQ_OK);
            lrig_settle(&r, "zdlb.stale", "row");
            drain_events(cl);
            static const zero_row_t ROWS[] = {
                { .cls = "zdlb.stale", .kind = MOQ_SIM_TRACE_DELAY_STALE,
                  .pin_input_kind = true,
                  .input_kind = MOQ_SIM_INPUT_DATA_RESET,
                  .pin_from = true, .from = MOQ_PERSPECTIVE_SERVER,
                  .pin_to = true, .to = MOQ_PERSPECTIVE_CLIENT,
                  .exact = true, .want_count = 1 },
                { .cls = "zdlb.enqueue.data_reset",
                  .kind = MOQ_SIM_TRACE_DELAY_ENQUEUE,
                  .pin_input_kind = true,
                  .input_kind = MOQ_SIM_INPUT_DATA_RESET,
                  .pin_from = true, .from = MOQ_PERSPECTIVE_SERVER,
                  .pin_to = true, .to = MOQ_PERSPECTIVE_CLIENT,
                  .exact = true, .want_count = 2 },
            };
            check_zero_rows(&r.log, mark, ROWS, 2);
        }
        CHECKN("zdlb.no_overflow", !r.log.overflow);
        CHECKN("zdlb.comparable",  !r.log.incomparable);
        moq_simpair_destroy(r.sp);
    }
    if (failures == before) printf("PASS: zero_detail_delay\n");
}

/* The fault producers: MUTATE, REORDER, TRUNCATE and all three inline
 * FAULT_INJECT branches. */
static void t_zero_detail_faults(void)
{
    int before = failures;

    printf("ZERO-DETAIL-FAULTS:\n");

    /* (a) mutate / reorder / truncate, driven by real traffic. */
    {
        lrig_t r;
        uint32_t flags = MOQ_SIM_FAULT_MUTATE_CONTROL |
                         MOQ_SIM_FAULT_MUTATE_DATA;
        if (zrig_up(&r, "zfa", MOQ_VERSION_DRAFT_16, flags, 1000, false) != 0) { CHECKN("zfa.rig", false); return; }
        (void)lrig_flow_quiet(&r);
        lrig_settle_quiet(&r);
        static const zero_row_t ROWS[] = {
            { .cls = "zfa.mutate", .kind = MOQ_SIM_TRACE_FAULT_MUTATE,
              .want_count = 1 },
        };
        check_zero_rows(&r.log, 0, ROWS, 1);
        CHECKN("zfa.no_overflow", !r.log.overflow);
        CHECKN("zfa.comparable",  !r.log.incomparable);
        moq_simpair_destroy(r.sp);
    }

    /* (a2) truncate on its own: with mutation also armed the truncate branch
     * consumes the same action first, so the two must not share a rig. */
    {
        lrig_t r;
        uint32_t flags = MOQ_SIM_FAULT_TRUNCATE_CONTROL |
                         MOQ_SIM_FAULT_TRUNCATE_DATA;
        if (zrig_up(&r, "zfa2", MOQ_VERSION_DRAFT_16, flags, 1000, false) != 0) {
            CHECKN("zfa2.rig", false); return;
        }
        (void)lrig_flow_quiet(&r);
        lrig_settle_quiet(&r);
        static const zero_row_t ROWS[] = {
            { .cls = "zfa2.truncate", .kind = MOQ_SIM_TRACE_FAULT_TRUNCATE,
              .want_count = 1 },
        };
        check_zero_rows(&r.log, 0, ROWS, 1);
        moq_simpair_destroy(r.sp);
    }

    /*
     * (b) reorder swaps an ADJACENT eligible pair, so the rig must leave two
     * SEND_DATA actions queued in ONE poll batch: two objects are written
     * back to back with no settle between them.
     */
    {
        lrig_t r;
        if (zrig_up(&r, "zfb", MOQ_VERSION_DRAFT_18, MOQ_SIM_FAULT_REORDER_ACTION, 1000, false) != 0) {
            CHECKN("zfb.rig", false); return;
        }
        lflow_t f = lrig_flow_quiet(&r);
        CHECKN("zfb.flow.ok", f.ok);
        if (f.ok) {
            moq_rcbuf_t *p = NULL;
            if (moq_rcbuf_create(moq_alloc_default(),
                                 (const uint8_t *)"ab", 2, &p) == MOQ_OK) {
                (void)moq_session_write_object(moq_simpair_server(r.sp),
                                               f.sg, 1, p, 1000);
                (void)moq_session_write_object(moq_simpair_server(r.sp),
                                               f.sg, 2, p, 1000);
                moq_rcbuf_decref(p);
            }
        }
        lrig_settle_quiet(&r);
        static const zero_row_t ROWS[] = {
            { .cls = "zfb.reorder", .kind = MOQ_SIM_TRACE_FAULT_REORDER,
              .want_count = 1 },
        };
        check_zero_rows(&r.log, 0, ROWS, 1);
        moq_simpair_destroy(r.sp);
    }

    /* (c) the three inline FAULT_INJECT branches, each pinned by its own
     * legacy identity so one cannot stand in for another. */
    {
        lrig_t r;
        if (zrig_up(&r, "zfc", MOQ_VERSION_DRAFT_18, MOQ_SIM_FAULT_INJECT_RESET, 1000, false) != 0) {
            CHECKN("zfc.rig", false); return;
        }
        (void)lrig_flow_quiet(&r);
        lrig_settle_quiet(&r);
        static const zero_row_t ROWS[] = {
            { .cls = "zfc.inject_reset", .kind = MOQ_SIM_TRACE_FAULT_INJECT,
              .pin_action_kind = true, .action_kind = MOQ_ACTION_RESET_DATA,
              .pin_code = true, .code = 0x100, .want_count = 1 },
        };
        check_zero_rows(&r.log, 0, ROWS, 1);
        moq_simpair_destroy(r.sp);
    }
    {
        lrig_t r;
        if (zrig_up(&r, "zfd", MOQ_VERSION_DRAFT_18, MOQ_SIM_FAULT_INJECT_STOP, 1000, false) != 0) {
            CHECKN("zfd.rig", false); return;
        }
        (void)lrig_flow_quiet(&r);
        lrig_settle_quiet(&r);
        static const zero_row_t ROWS[] = {
            { .cls = "zfd.inject_stop", .kind = MOQ_SIM_TRACE_FAULT_INJECT,
              .pin_action_kind = true, .action_kind = MOQ_ACTION_STOP_DATA,
              .pin_code = true, .code = 0x100, .want_count = 1 },
        };
        check_zero_rows(&r.log, 0, ROWS, 1);
        moq_simpair_destroy(r.sp);
    }
    {
        lrig_t r;
        if (zrig_up(&r, "zfe", MOQ_VERSION_DRAFT_18, MOQ_SIM_FAULT_INJECT_CLOSE, 1000, false) != 0) {
            CHECKN("zfe.rig", false); return;
        }
        /*
         * ONE pump step, deliberately: the injection fires once per direction
         * per step, so a single step gives an EXACT, declarable image -- one
         * FAULT_INJECT record and one CONTROL INPUT per direction, and the
         * rows below pin the client-to-server pair as exactly one each.
         * Running the whole flow instead produced 124 injections and 123
         * injected CONTROL inputs, a count that is an artifact of how many
         * times the pump happened to run and cannot honestly be pinned.
         */
        size_t mark = r.log.n;   /* post-handshake: only injected control here */
        CHECKN("zfe.step", moq_simpair_step(r.sp, NULL) == MOQ_OK);
        /*
         * The injected close is TWO producers at one site: the inline
         * FAULT_INJECT record, and the separate trace_input() call that
         * reports the malformed envelope's delivery. The second is the eighth
         * SIM_INPUT_DETAIL_NONE call site and needs its own row -- checking
         * only the FAULT_INJECT record leaves it unconstrained.
         *
         * The envelope is declared here, independently of the producer:
         * QUIC varint 255 (unknown message type) + a zero uint16 length.
         */
        static const uint8_t BAD_MSG[4] = { 0x40, 0xFF, 0x00, 0x00 };
        static const zero_row_t ROWS[] = {
            /* the two producers at this site, as a declared ONE-TO-ONE image */
            { .cls = "zfe.inject_close", .kind = MOQ_SIM_TRACE_FAULT_INJECT,
              .pin_action_kind = true, .action_kind = MOQ_ACTION_CLOSE_SESSION,
              .pin_code = true, .code = 0x3,
              .pin_from = true, .from = MOQ_PERSPECTIVE_CLIENT,
              .pin_to = true, .to = MOQ_PERSPECTIVE_SERVER,
              .exact = true, .want_count = 1 },
            { .cls = "zfe.inject_close.input", .kind = MOQ_SIM_TRACE_INPUT,
              .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_CONTROL_BYTES,
              .pin_from = true, .from = MOQ_PERSPECTIVE_CLIENT,
              .pin_to = true, .to = MOQ_PERSPECTIVE_SERVER,
              /* the real delivery reports the protocol close as a SUCCESSFUL
               * transition; the bytes are the declared envelope, exactly */
              .want_result_set = true, .want_result = MOQ_OK,
              .want_bytes = BAD_MSG, .want_bytes_len = sizeof(BAD_MSG),
              .exact = true, .want_count = 1 },
        };
        check_zero_rows(&r.log, mark, ROWS, 2);
        moq_simpair_destroy(r.sp);
    }
    if (failures == before) printf("PASS: zero_detail_faults\n");
}

/* ===================================================================== *
 * Capture-guard self-check (quiet unless it fails)
 *
 * The two invalid span shapes must make a capture INCOMPARABLE rather than
 * silently producing a zero-filled comparison a row could then "match".
 * Both spans -- the legacy one and the appended one -- are guarded the same
 * way, so both are pinned here.
 * ===================================================================== */
static void t_capture_guard(void)
{
    static uint8_t big[CAP_BYTES + 8];
    moq_sim_trace_record_t r;
    cap_log_t l;

    memset(big, 0xAB, sizeof(big));

    /* a well-formed record must NOT be flagged */
    cap_reset(&l);
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.kind = MOQ_SIM_TRACE_INPUT;
    r.bytes.data = big; r.bytes.len = 4;
    cap_fn(&l, &r);
    CHECKN("guard.valid_is_comparable", !l.incomparable && l.n == 1);
    CHECKN("guard.valid_bytes_copied", memcmp(l.recs[0].bytes, big, 4) == 0);

    /* oversize legacy span */
    cap_reset(&l);
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.bytes.data = big; r.bytes.len = CAP_BYTES + 1;
    cap_fn(&l, &r);
    CHECKN("guard.legacy_oversize", l.incomparable);

    /* nonzero legacy length with a NULL pointer */
    cap_reset(&l);
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.bytes.data = NULL; r.bytes.len = 3;
    cap_fn(&l, &r);
    CHECKN("guard.legacy_null_nonzero", l.incomparable);

    /* the same two shapes on the appended span */
    cap_reset(&l);
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.detail_bytes.data = big; r.detail_bytes.len = CAP_BYTES + 1;
    cap_fn(&l, &r);
    CHECKN("guard.detail_oversize", l.incomparable);

    cap_reset(&l);
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.detail_bytes.data = NULL; r.detail_bytes.len = 3;
    cap_fn(&l, &r);
    CHECKN("guard.detail_null_nonzero", l.incomparable);
}

/*
 * Quiet self-check: a partially declared or unsafe row must be REFUSED, never
 * passed over. The rejection classes exercised below are, in order: no class
 * name; an empty class name; a zero kind; an unknown kind; the two
 * nonzero-detail kinds (ACTION, FAULT_DROP) misused in this matrix; a zero
 * required count; a byte pointer with no length; a length with no byte
 * pointer; a byte length past the capture buffer; an invalid pinned INPUT
 * subtype; and an invalid pinned action subtype.
 *
 * The accepted rows carry REAL backing storage for every declared span. The
 * validator checks the numeric length against CAP_BYTES; it cannot discover an
 * allocation's size, and must not try to -- the C contract is
 * pointer-valid-for-length, so the fixture owes a buffer that actually holds
 * what it declares. B_CAP exists for exactly the at-capacity row.
 */
static void t_zero_row_descriptor(void)
{
    static const uint8_t B[2] = { 1, 2 };
    static const uint8_t B_CAP[CAP_BYTES] = { 0 };
    _Static_assert(sizeof(B_CAP) == CAP_BYTES,
                   "the at-capacity descriptor must be backed by CAP_BYTES");
    /* Each rejected row is named so a validator that stops rejecting one
     * shape is caught by ITS case, not by an aggregate count. */
    static const struct { const char *name; zero_row_t row; } BAD[] = {
        { "no_class",
          { .cls = NULL, .kind = MOQ_SIM_TRACE_INPUT, .want_count = 1 } },
        { "empty_class",
          { .cls = "", .kind = MOQ_SIM_TRACE_INPUT, .want_count = 1 } },
        { "zero_kind",
          { .cls = "d.zero_kind", .kind = 0, .want_count = 1 } },
        { "unknown_kind",
          { .cls = "d.unknown_kind", .kind = (moq_sim_trace_kind_t)0x7f,
            .want_count = 1 } },
        { "action_misuse",
          { .cls = "d.action", .kind = MOQ_SIM_TRACE_ACTION,
            .want_count = 1 } },
        { "fault_drop_misuse",
          { .cls = "d.fault_drop", .kind = MOQ_SIM_TRACE_FAULT_DROP,
            .want_count = 1 } },
        { "zero_count",
          { .cls = "d.zero_count", .kind = MOQ_SIM_TRACE_INPUT,
            .want_count = 0 } },
        { "bytes_without_len",
          { .cls = "d.bytes_no_len", .kind = MOQ_SIM_TRACE_INPUT,
            .want_bytes = B, .want_bytes_len = 0, .want_count = 1 } },
        { "len_without_bytes",
          { .cls = "d.len_no_bytes", .kind = MOQ_SIM_TRACE_INPUT,
            .want_bytes = NULL, .want_bytes_len = 2, .want_count = 1 } },
        /* deliberately an INVALID span (length past the object AND past the
         * capture buffer): the validator must refuse it, so it is never
         * handed to a comparison */
        { "bytes_oversize",
          { .cls = "d.bytes_oversize", .kind = MOQ_SIM_TRACE_INPUT,
            .want_bytes = B, .want_bytes_len = CAP_BYTES + 1,
            .want_count = 1 } },
        { "bad_input_kind",
          { .cls = "d.bad_input_kind", .kind = MOQ_SIM_TRACE_INPUT,
            .pin_input_kind = true,
            .input_kind = (moq_sim_trace_input_kind_t)0x5a,
            .want_count = 1 } },
        { "bad_action_kind",
          { .cls = "d.bad_action_kind", .kind = MOQ_SIM_TRACE_FAULT_INJECT,
            .pin_action_kind = true, .action_kind = (moq_action_kind_t)0x5a,
            .want_count = 1 } },
    };
    /* One complete VALID row per legal pin shape, so a validator cannot pass
     * this self-check by rejecting everything. */
    static const struct { const char *name; zero_row_t row; } GOOD[] = {
        { "plain",
          { .cls = "d.ok", .kind = MOQ_SIM_TRACE_QUIESCENT, .want_count = 1 } },
        { "input_kind",
          { .cls = "d.ok.input", .kind = MOQ_SIM_TRACE_INPUT,
            .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_TICK,
            .exact = true, .want_count = 2 } },
        { "action_kind",
          { .cls = "d.ok.action", .kind = MOQ_SIM_TRACE_FAULT_INJECT,
            .pin_action_kind = true, .action_kind = MOQ_ACTION_CLOSE_SESSION,
            .pin_code = true, .code = 0x3, .want_count = 1 } },
        { "bytes",
          { .cls = "d.ok.bytes", .kind = MOQ_SIM_TRACE_INPUT,
            .pin_from = true, .from = MOQ_PERSPECTIVE_CLIENT,
            .pin_to = true, .to = MOQ_PERSPECTIVE_SERVER,
            .want_result_set = true, .want_result = MOQ_OK,
            .want_bytes = B, .want_bytes_len = sizeof(B),
            .want_count = 1 } },
        { "bytes_at_capacity",
          { .cls = "d.ok.bytes_cap", .kind = MOQ_SIM_TRACE_INPUT,
            .want_bytes = B_CAP, .want_bytes_len = CAP_BYTES,
            .want_count = 1 } },
        { "delay_kinds",
          { .cls = "d.ok.stale", .kind = MOQ_SIM_TRACE_DELAY_STALE,
            .pin_input_kind = true, .input_kind = MOQ_SIM_INPUT_DATA_RESET,
            .want_count = 1 } },
    };
    char nm[96];

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        snprintf(nm, sizeof(nm), "desc.rejects.%s", BAD[i].name);
        CHECKN(nm, !zero_row_valid(&BAD[i].row));
    }
    for (size_t i = 0; i < sizeof(GOOD) / sizeof(GOOD[0]); i++) {
        snprintf(nm, sizeof(nm), "desc.accepts.%s", GOOD[i].name);
        CHECKN(nm, zero_row_valid(&GOOD[i].row));
    }
}

int main(void)
{
    t_capture_guard();
    t_zero_row_descriptor();
    /* compatibility first: if the prefix moved, everything else is moot */
    t_v0_prefix_frozen();
    t_v0_prefix_frozen_data();
    t_struct_size();

    /* then the appended detail */
    t_action_rows();
    t_close_reason();
    t_input_matrix(false);
    t_input_matrix(true);

    /* every kind the contract declares to carry NO appended detail */
    t_zero_detail_input();
    t_zero_detail_delay();
    t_zero_detail_faults();

    if (failures == 0)
        printf("PASS: sim_trace_contract\n");
    else
        fprintf(stderr, "FAIL: sim_trace_contract (%d)\n", failures);
    return failures != 0;
}
