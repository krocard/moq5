#ifndef MOQR_CLI_BLOCKEDSTATS_H
#define MOQR_CLI_BLOCKEDSTATS_H

/*
 * Blocked-heavy reason record (RELAY_BLOCKED_V0). One row PER LANE, rendered
 * by the coordinator once every lane has published its blocked aggregate for
 * a given SIGUSR1 dump epoch — so a complete record is a same-epoch,
 * ascending-lane set of exactly K rows. The counters are read LIVE on the
 * owning lane (clients connected, parked state intact); a stop-time read
 * would see detach-cleared parked state and report zeros.
 *
 * The formatter/parser/validators are pure over this plain struct (no bind
 * or adapter types), so the grammar and the completeness/cross-field/
 * per-mode acceptance rules are testable in any tree. A lane whose collection
 * failed (overflow/getter refusal) emits an explicit refusal row that
 * invalidates the whole epoch — never zeros that look valid.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOQR_BLOCKED_PREFIX "RELAY_BLOCKED_V0"

/* An all-maximum row (every u64 = 20 digits, every u32 = 10 digits) is 337
 * bytes + NUL; 512 leaves headroom and is the ONE size every caller must use
 * for a per-row buffer, so a legal maxed row never truncates. Pinned by
 * test_relay_blocked_stats (all-max row must fit and round-trip). */
#define MOQR_BLOCKED_ROW_MAX 512u

/* One lane's live blocked aggregate for one dump epoch. conns_* / *_total are
 * historical (counters since connect); parked_* are the CURRENT parked
 * populations by reason (the exceptional set). All over LIVE conns only. */
typedef struct moqr_cli_blocked_row {
    uint64_t epoch;
    uint32_t lane;
    uint32_t live_conns;
    uint32_t conns_action_cap;
    uint32_t conns_session_sg;
    uint32_t conns_bind_sg;
    uint64_t action_cap_total;
    uint64_t session_sg_total;
    uint64_t bind_sg_total;
    uint32_t parked_action_cap;
    uint32_t parked_session_sg;
} moqr_cli_blocked_row_t;

/* Which blockage a scenario exercises (the blocked binding is on lane 0). */
typedef enum moqr_blocked_mode {
    MOQR_BLK_ACTION_CAP = 0,
    MOQR_BLK_SESSION_SG = 1,
    MOQR_BLK_BIND_SG = 2,
} moqr_blocked_mode_t;

typedef enum moqr_cli_blocked_kind {
    MOQR_BLOCKED_MALFORMED = 0,
    MOQR_BLOCKED_ROW = 1,
    MOQR_BLOCKED_REFUSED = 2,
} moqr_cli_blocked_kind_t;

/* Serialize one row. Returns bytes written (< n) or negative on truncation;
 * the trailing ,eor=1 makes a mid-write truncation detectable as malformed. */
int moqr_cli_blocked_format(char *buf, size_t n,
                            const moqr_cli_blocked_row_t *row);

/* Serialize an explicit refusal for a lane whose collection failed. */
int moqr_cli_blocked_format_refused(char *buf, size_t n, uint64_t epoch,
                                    uint32_t lane);

/* Parse one line. On MOQR_BLOCKED_ROW, *out is filled; on REFUSED, only
 * out->epoch/out->lane are meaningful; MALFORMED leaves *out unspecified. */
moqr_cli_blocked_kind_t moqr_cli_blocked_parse(const char *line,
                                               moqr_cli_blocked_row_t *out);

/* Per-row cross-field self-consistency (independent of mode):
 *  - every conns_* and parked_* <= live_conns;
 *  - parked_action_cap <= conns_action_cap; parked_session_sg <=
 *    conns_session_sg (a current park implies the reason was reached);
 *  - each *_total == 0 iff its conns_* == 0. */
bool moqr_cli_blocked_row_consistent(const moqr_cli_blocked_row_t *row);

/* ---- RELAY_SGDIAG: per-conn stranding diagnostics (verify build only) ----
 * One row per ACTIVE conn slot of a lane (quiet slots are suppressed at the
 * emitter). occ = downstream subgroup-pool occupancy; ready/parked/reason =
 * the delivery-scheduler state; the three *_total counters are that slot's
 * HISTORICAL blockage counts — the blocked connection is identified by
 * counters, never inferred: acceptance requires EXACTLY ONE row with
 * bind_sg_total > 0 (zero or several = ambiguous target, fail closed). */
#define MOQR_SGDIAG_PREFIX "RELAY_SGDIAG"

typedef struct moqr_cli_sgdiag_row {
    uint64_t epoch;
    uint32_t lane;
    uint32_t slot;
    uint32_t occ;
    uint32_t ready;    /* 0/1 */
    uint32_t parked;   /* 0/1 */
    uint32_t reason;   /* 0 none / 1 action-cap / 2 session-sg */
    uint64_t action_cap_total;
    uint64_t session_sg_total;
    uint64_t bind_sg_total;
    /* BIND_SG demand attribution: which demand's delivery hit this conn's
     * subgroup-pool refusal (the FIRST refusal latches; 0 = none recorded).
     * bind_sg_demand_ambiguous = a later refusal named a DIFFERENT demand —
     * the SGDIAG→SEALLOG join must then fail closed, never guess. */
    uint64_t bind_sg_demand;
    uint32_t bind_sg_demand_ambiguous;   /* 0/1 */
} moqr_cli_sgdiag_row_t;

int moqr_cli_sgdiag_format(char *buf, size_t n,
                           const moqr_cli_sgdiag_row_t *row);
/* MALFORMED / ROW only (diagnostics have no refusal shape). */
moqr_cli_blocked_kind_t moqr_cli_sgdiag_parse(const char *line,
                                              moqr_cli_sgdiag_row_t *out);
/* Field validity: ready/parked/ambiguous <= 1, reason <= 2, parked implies
 * reason, and a demand or ambiguity implies the refusal was reached
 * (bind_sg_total > 0). */
bool moqr_cli_sgdiag_row_consistent(const moqr_cli_sgdiag_row_t *row);
/* Exactly-one-blocked-target acceptance over one lane's rows: nonzero
 * `epoch` on every row, every row from the EXPECTED lane (the blocked
 * binding's destination — wrong-lane rows are someone else's evidence and
 * fail closed), no duplicate slot, every row consistent, and EXACTLY ONE
 * row with bind_sg_total > 0 — its slot lands in *out_slot and its demand
 * attribution in *out_demand. The blocked row must IDENTIFY its demand:
 * bind_sg_demand == 0 (never recorded) or bind_sg_demand_ambiguous == 1
 * (two demands refused on the same conn) fail closed — the join from the
 * blocked connection to its SEALLOG seals must be exact, never a guess. */
bool moqr_cli_sgdiag_accept(const moqr_cli_sgdiag_row_t *rows, size_t count,
                            uint64_t epoch, uint32_t lane, uint32_t *out_slot,
                            uint64_t *out_demand, const char **why);

/* ---- RELAY_SEALLOG: destination SG_SEAL ingest evidence (verify only) ----
 * One row per ingested seal, in INGEST ORDER: seq is the destination
 * shard's monotonically increasing ingest sequence (per lane), src the
 * owner lane the seal crossed from. Bounded at the recorder (a ring), so a
 * long run keeps the newest window. */
#define MOQR_SEALLOG_PREFIX "RELAY_SEALLOG"

typedef struct moqr_cli_seallog_row {
    uint64_t epoch;
    uint32_t lane;      /* destination (emitting) lane                     */
    uint64_t seq;       /* ingest order on that lane                       */
    uint64_t total;     /* LIFETIME ingest count at emission — total >
                         * window size means the ring overwrote evidence   */
    uint32_t src;       /* owner lane                                      */
    uint64_t demand;    /* exact demand attribution (background XL seals
                         * for the same group/subgroup cannot collide)     */
    uint64_t group_id;
    uint64_t subgroup_id;
} moqr_cli_seallog_row_t;

int moqr_cli_seallog_format(char *buf, size_t n,
                            const moqr_cli_seallog_row_t *row);
moqr_cli_blocked_kind_t moqr_cli_seallog_parse(const char *line,
                                               moqr_cli_seallog_row_t *out);
/* How many accepted rows carry `demand` AT OR AFTER `since_seq` — the
 * scenario's blocked demand must own its expected seal count INSIDE the
 * accepted interval. `since_seq` is the interval cursor (the baseline
 * epoch's previous_total; 0 = the whole window): a retained ring can hold
 * mapped-demand seals from BEFORE the cursor, and counting those would let
 * stale history satisfy the target — pre-cursor rows never count and never
 * establish seal presence. A same-(group,subgroup) seal from a DIFFERENT
 * demand is visibly distinct, never merged. */
size_t moqr_cli_seallog_count_demand(const moqr_cli_seallog_row_t *rows,
                                     size_t count, uint64_t demand,
                                     uint64_t since_seq);

/* ---- RELAY_SEALMETA: per-lane seal-ring window metadata (verify only) ----
 * The seal ring keeps only the newest window (32), so lifetime completeness
 * (retained count == lifetime total) is IMPOSSIBLE once the ring wraps — the
 * old count==total acceptance goes permanently invalid after the 33rd seal.
 * Metadata makes evidence INTERVAL-complete instead of lifetime-complete: it
 * pins the retained window's lower edge and the lifetime total so a later
 * snapshot can prove it still covers a previously established cursor.
 *
 * One row per lane, emitted on EVERY dump — even when retained_count == 0 (a
 * zero-event snapshot is still evidence: the interval since the cursor is
 * empty and the lifetime total is unchanged). It is the seal record's header;
 * the RELAY_SEALLOG rows are its body. */
#define MOQR_SEALMETA_PREFIX "RELAY_SEALMETA"

typedef struct moqr_cli_seallog_meta {
    uint64_t epoch;
    uint32_t lane;               /* destination (emitting) lane              */
    uint64_t first_retained_seq; /* oldest seq still in the ring window;
                                  * == lifetime_total when retained_count==0 */
    uint64_t lifetime_total;     /* total seals ingested on this lane, ever  */
    uint64_t retained_count;     /* rows the ring still holds this dump       */
} moqr_cli_seallog_meta_t;

int moqr_cli_seallog_meta_format(char *buf, size_t n,
                                 const moqr_cli_seallog_meta_t *meta);
moqr_cli_blocked_kind_t
moqr_cli_seallog_meta_parse(const char *line, moqr_cli_seallog_meta_t *out);

/* Interval-complete acceptance for ONE lane's seal evidence. `meta` is
 * REQUIRED (NULL fails closed — a snapshot with no header proves nothing).
 *
 * Self-consistency (checked always): meta.lane == lane, meta.epoch == epoch
 * (nonzero); meta.retained_count == count; the `rows` are EXACTLY the
 * contiguous interval [first_retained_seq, lifetime_total) — each seq present
 * once in ascending order, none missing, none outside, all carrying the
 * expected lane/epoch and lifetime_total; first_retained_seq <= lifetime_total;
 * and when retained_count == 0, first_retained_seq == lifetime_total with no
 * rows. Any gap, reorder, wrong lane, mixed epoch, or meta/row disagreement
 * fails closed.
 *
 * Cursor semantics: `is_baseline` marks the FIRST epoch, which accepts any
 * self-consistent window and hands back its lifetime_total via *out_total as
 * the cursor for later epochs (history before that cursor need not survive).
 * A LATER epoch is interval-complete iff the ring did NOT overwrite the
 * cursor (previous_total >= first_retained_seq) AND the whole interval
 * [previous_total, lifetime_total) is present (guaranteed once the window is
 * contiguous and covers the cursor). previous_total < first_retained_seq means
 * the interval since the cursor was OVERWRITTEN -> reject; previous_total >
 * lifetime_total means the cursor ran ahead of the total (impossible) ->
 * reject. On success *out_total (if non-NULL) advances to lifetime_total. */
bool moqr_cli_seallog_accept_interval(
    const moqr_cli_seallog_meta_t *meta,
    const moqr_cli_seallog_row_t *rows, size_t count, uint64_t epoch,
    uint32_t lane, uint64_t previous_total, bool is_baseline,
    uint64_t *out_total, const char **why);

/* ---- Pre-registered BIND_SG stranding decision table (verify only) ----
 * Maps one observation of the blocked connection — did the EXPECTED seal
 * appear in an interval-complete SEALLOG window, and what do the SGDIAG
 * occ/ready fields say — to its ONE diagnosis. Registered BEFORE the
 * real-transport 4/6 investigation so a reading maps to a pre-committed
 * interpretation, never a post-hoc story:
 *
 *   seal | occ vs cap | ready | diagnosis
 *   -----+------------+-------+-------------------------------------------
 *   no   |    —       |   —   | NOT_APPLIED: the seal did not reach
 *        |            |       | destination seal application (upstream of
 *        |            |       | the bind entirely) — only valid over an
 *        |            |       | interval-COMPLETE window (an incomplete
 *        |            |       | window proves nothing).
 *   yes  |  occ==cap  |   0   | NO_RELEASE: seal applied but no bind
 *        |            |       | readiness/slot release followed.
 *   yes  |  occ==cap  |   1   | PASS_INCOMPLETE: the binding is ready but
 *        |            |       | another delivery pass has not completed.
 *   yes  |  occ<cap   |   0   | NOT_REARMED: the slot released but the
 *        |            |       | retained work was not reselected/rearmed
 *        |            |       | (meaningful while delivery is still short,
 *        |            |       | e.g. stuck at 4/6).
 *   yes  |  occ<cap   |   1   | DOORBELL_UNCONSUMED: ready work exists but
 *        |            |       | the pump/doorbell has not consumed it
 *        |            |       | (meaningful when it PERSISTS across pumps).
 *
 * occ > cap is impossible by construction and maps to INVALID (fail closed).
 * The table interprets observations; it prescribes NO scheduling change. */
typedef enum moqr_sg_diagnosis {
    MOQR_SG_DIAG_INVALID = 0,
    MOQR_SG_DIAG_NOT_APPLIED = 1,
    MOQR_SG_DIAG_NO_RELEASE = 2,
    MOQR_SG_DIAG_PASS_INCOMPLETE = 3,
    MOQR_SG_DIAG_NOT_REARMED = 4,
    MOQR_SG_DIAG_DOORBELL_UNCONSUMED = 5,
} moqr_sg_diagnosis_t;

moqr_sg_diagnosis_t moqr_cli_sg_diagnose(bool seal_present, uint32_t occ,
                                         uint32_t cap, uint32_t ready);
/* The pre-registered wording for a diagnosis (static string, never NULL). */
const char *moqr_cli_sg_diagnosis_str(moqr_sg_diagnosis_t d);

/* Accept a full record: exactly `lanes` ROW lines (no refusals), lane index
 * 0..lanes-1 each exactly once in ASCENDING order, all carrying `epoch`
 * (nonzero), every row consistent, and the per-mode acceptance for the
 * blocked binding on lane 0:
 *   ACTION_CAP  : lane0 conns_action_cap==1, action_cap_total>=1,
 *                 parked_action_cap==1; no other lane has conns_action_cap>0.
 *   SESSION_SG  : lane0 conns_session_sg==1, session_sg_total>=1,
 *                 parked_session_sg==1; no other lane has conns_session_sg>0.
 *   BIND_SG     : lane0 conns_bind_sg==1, bind_sg_total>=1; every lane has
 *                 parked_action_cap==0 && parked_session_sg==0 (BIND_SG never
 *                 parks); no other lane has conns_bind_sg>0.
 * On failure returns false and (if why!=NULL) sets *why to a static reason. */
bool moqr_cli_blocked_accept(const moqr_cli_blocked_row_t *rows, size_t count,
                             uint32_t lanes, uint64_t epoch,
                             moqr_blocked_mode_t mode, const char **why);

#ifdef __cplusplus
}
#endif

#endif /* MOQR_CLI_BLOCKEDSTATS_H */
