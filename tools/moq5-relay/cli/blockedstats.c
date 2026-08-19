#include "blockedstats.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Field order is fixed and the parser requires exactly this sequence; the
 * trailing eor=1 catches a mid-write truncation (a value cut short would
 * otherwise parse as a shorter valid integer). */

int
moqr_cli_blocked_format(char *buf, size_t n,
                        const moqr_cli_blocked_row_t *r)
{
    if (buf == NULL || r == NULL) {
        return -1;
    }
    int w = snprintf(
        buf, n,
        MOQR_BLOCKED_PREFIX
        ",epoch=%" PRIu64 ",lane=%u,live_conns=%u"
        ",conns_action_cap=%u,conns_session_sg=%u,conns_bind_sg=%u"
        ",action_cap_total=%" PRIu64 ",session_sg_total=%" PRIu64
        ",bind_sg_total=%" PRIu64
        ",parked_action_cap=%u,parked_session_sg=%u,eor=1",
        r->epoch, r->lane, r->live_conns, r->conns_action_cap,
        r->conns_session_sg, r->conns_bind_sg, r->action_cap_total,
        r->session_sg_total, r->bind_sg_total, r->parked_action_cap,
        r->parked_session_sg);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

int
moqr_cli_blocked_format_refused(char *buf, size_t n, uint64_t epoch,
                                uint32_t lane)
{
    if (buf == NULL) {
        return -1;
    }
    int w = snprintf(buf, n,
                     MOQR_BLOCKED_PREFIX ",epoch=%" PRIu64
                                        ",lane=%u,refused=1,eor=1",
                     epoch, lane);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

/* Parse "key=<u64>" strictly: the token must start with key, and the value
 * must be all digits (no sign, no junk) fitting u64. Returns false on any
 * mismatch. */
static bool
kv_u64(const char *tok, const char *key, uint64_t *out)
{
    size_t kl = strlen(key);
    if (strncmp(tok, key, kl) != 0 || tok[kl] != '=') {
        return false;
    }
    const char *v = tok + kl + 1;
    if (*v == '\0') {
        return false;
    }
    uint64_t acc = 0;
    for (const char *c = v; *c; c++) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        uint64_t d = (uint64_t)(*c - '0');
        if (acc > (UINT64_MAX - d) / 10u) {
            return false;   /* overflow */
        }
        acc = acc * 10u + d;
    }
    *out = acc;
    return true;
}

static bool
kv_u32(const char *tok, const char *key, uint32_t *out)
{
    uint64_t v;
    if (!kv_u64(tok, key, &v) || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

moqr_cli_blocked_kind_t
moqr_cli_blocked_parse(const char *line, moqr_cli_blocked_row_t *out)
{
    if (line == NULL || out == NULL) {
        return MOQR_BLOCKED_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    size_t plen = strlen(MOQR_BLOCKED_PREFIX);
    if (strncmp(line, MOQR_BLOCKED_PREFIX, plen) != 0 || line[plen] != ',') {
        return MOQR_BLOCKED_MALFORMED;
    }
    /* Split the remainder on ',' PRESERVING empty tokens: a doubled comma or a
     * trailing comma yields a zero-length token that fails every key match /
     * shifts the token count, so RELAY_BLOCKED_V0,,epoch=... and a trailing
     * comma are both rejected (strtok would silently collapse them). */
    char work[MOQR_BLOCKED_ROW_MAX];
    if (strlen(line) >= sizeof(work)) {
        return MOQR_BLOCKED_MALFORMED;
    }
    strcpy(work, line + plen + 1);
    char *toks[24];
    size_t nt = 0;
    toks[nt++] = work;
    for (char *p = work; *p != '\0'; p++) {
        if (*p == ',') {
            *p = '\0';
            if (nt >= 24) {
                return MOQR_BLOCKED_MALFORMED;
            }
            toks[nt++] = p + 1;
        }
    }
    /* Refusal record: epoch, lane, refused=1, eor=1 (exactly 4 tokens). */
    if (nt == 4 && kv_u64(toks[0], "epoch", &out->epoch) &&
        kv_u32(toks[1], "lane", &out->lane) &&
        strcmp(toks[2], "refused=1") == 0 && strcmp(toks[3], "eor=1") == 0) {
        return MOQR_BLOCKED_REFUSED;
    }
    /* Full row: exactly 12 tokens in fixed order + eor. */
    if (nt != 12) {
        return MOQR_BLOCKED_MALFORMED;
    }
    uint64_t ac_t, sg_t, bs_t;
    if (kv_u64(toks[0], "epoch", &out->epoch) &&
        kv_u32(toks[1], "lane", &out->lane) &&
        kv_u32(toks[2], "live_conns", &out->live_conns) &&
        kv_u32(toks[3], "conns_action_cap", &out->conns_action_cap) &&
        kv_u32(toks[4], "conns_session_sg", &out->conns_session_sg) &&
        kv_u32(toks[5], "conns_bind_sg", &out->conns_bind_sg) &&
        kv_u64(toks[6], "action_cap_total", &ac_t) &&
        kv_u64(toks[7], "session_sg_total", &sg_t) &&
        kv_u64(toks[8], "bind_sg_total", &bs_t) &&
        kv_u32(toks[9], "parked_action_cap", &out->parked_action_cap) &&
        kv_u32(toks[10], "parked_session_sg", &out->parked_session_sg) &&
        strcmp(toks[11], "eor=1") == 0) {
        out->action_cap_total = ac_t;
        out->session_sg_total = sg_t;
        out->bind_sg_total = bs_t;
        return MOQR_BLOCKED_ROW;
    }
    return MOQR_BLOCKED_MALFORMED;
}

bool
moqr_cli_blocked_row_consistent(const moqr_cli_blocked_row_t *r)
{
    if (r == NULL) {
        return false;
    }
    uint32_t lc = r->live_conns;
    if (r->conns_action_cap > lc || r->conns_session_sg > lc ||
        r->conns_bind_sg > lc || r->parked_action_cap > lc ||
        r->parked_session_sg > lc) {
        return false;   /* population cannot exceed live conns */
    }
    if (r->parked_action_cap > r->conns_action_cap ||
        r->parked_session_sg > r->conns_session_sg) {
        return false;   /* a current park implies the reason was reached */
    }
    /* a conn parks under exactly one reason, so the two parked populations
     * are disjoint and their sum cannot exceed the live conn count (widened
     * to u64 so the addition itself cannot wrap) */
    if ((uint64_t)r->parked_action_cap + (uint64_t)r->parked_session_sg >
        (uint64_t)lc) {
        return false;
    }
    /* each total == 0 iff its conn count == 0 */
    if ((r->action_cap_total == 0) != (r->conns_action_cap == 0)) {
        return false;
    }
    if ((r->session_sg_total == 0) != (r->conns_session_sg == 0)) {
        return false;
    }
    if ((r->bind_sg_total == 0) != (r->conns_bind_sg == 0)) {
        return false;
    }
    return true;
}

/* Shared strict tokenizer for the two diagnostic records: split `line`
 * (after `prefix`,) on ',' preserving empty tokens; exactly `want` tokens
 * with the last being eor=1. Returns tokens in toks[] over `work`. */
static bool
diag_tokens(const char *line, const char *prefix, char *work, size_t work_n,
            char **toks, size_t want)
{
    if (line == NULL) {
        return false;
    }
    size_t plen = strlen(prefix);
    if (strncmp(line, prefix, plen) != 0 || line[plen] != ',') {
        return false;
    }
    if (strlen(line) >= work_n) {
        return false;
    }
    strcpy(work, line + plen + 1);
    size_t nt = 0;
    toks[nt++] = work;
    for (char *p = work; *p != '\0'; p++) {
        if (*p == ',') {
            *p = '\0';
            if (nt >= want) {
                return false;   /* too many tokens: toks[] holds `want` */
            }
            toks[nt++] = p + 1;
        }
    }
    return nt == want && strcmp(toks[want - 1], "eor=1") == 0;
}

int
moqr_cli_sgdiag_format(char *buf, size_t n, const moqr_cli_sgdiag_row_t *r)
{
    if (buf == NULL || r == NULL) {
        return -1;
    }
    int w = snprintf(
        buf, n,
        MOQR_SGDIAG_PREFIX ",epoch=%" PRIu64
        ",lane=%u,slot=%u,occ=%u,ready=%u,parked=%u,reason=%u"
        ",action_cap_total=%" PRIu64 ",session_sg_total=%" PRIu64
        ",bind_sg_total=%" PRIu64 ",bind_sg_demand=%" PRIu64
        ",bind_sg_demand_ambiguous=%u,eor=1",
        r->epoch, r->lane, r->slot, r->occ, r->ready, r->parked, r->reason,
        r->action_cap_total, r->session_sg_total, r->bind_sg_total,
        r->bind_sg_demand, r->bind_sg_demand_ambiguous);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

moqr_cli_blocked_kind_t
moqr_cli_sgdiag_parse(const char *line, moqr_cli_sgdiag_row_t *out)
{
    if (out == NULL) {
        return MOQR_BLOCKED_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    char work[MOQR_BLOCKED_ROW_MAX];
    char *t[13];
    if (!diag_tokens(line, MOQR_SGDIAG_PREFIX, work, sizeof(work), t, 13)) {
        return MOQR_BLOCKED_MALFORMED;
    }
    if (kv_u64(t[0], "epoch", &out->epoch) &&
        kv_u32(t[1], "lane", &out->lane) &&
        kv_u32(t[2], "slot", &out->slot) &&
        kv_u32(t[3], "occ", &out->occ) &&
        kv_u32(t[4], "ready", &out->ready) &&
        kv_u32(t[5], "parked", &out->parked) &&
        kv_u32(t[6], "reason", &out->reason) &&
        kv_u64(t[7], "action_cap_total", &out->action_cap_total) &&
        kv_u64(t[8], "session_sg_total", &out->session_sg_total) &&
        kv_u64(t[9], "bind_sg_total", &out->bind_sg_total) &&
        kv_u64(t[10], "bind_sg_demand", &out->bind_sg_demand) &&
        kv_u32(t[11], "bind_sg_demand_ambiguous",
               &out->bind_sg_demand_ambiguous)) {
        return MOQR_BLOCKED_ROW;
    }
    return MOQR_BLOCKED_MALFORMED;
}

bool
moqr_cli_sgdiag_row_consistent(const moqr_cli_sgdiag_row_t *r)
{
    if (r == NULL) {
        return false;
    }
    if (r->ready > 1 || r->parked > 1 || r->reason > 2 ||
        r->bind_sg_demand_ambiguous > 1) {
        return false;
    }
    if (r->parked == 1 && r->reason == 0) {
        return false;   /* a park always names its reason */
    }
    if ((r->bind_sg_demand != 0 || r->bind_sg_demand_ambiguous != 0) &&
        r->bind_sg_total == 0) {
        return false;   /* attribution implies the refusal was reached */
    }
    return true;
}

int
moqr_cli_seallog_format(char *buf, size_t n, const moqr_cli_seallog_row_t *r)
{
    if (buf == NULL || r == NULL) {
        return -1;
    }
    int w = snprintf(buf, n,
                     MOQR_SEALLOG_PREFIX ",epoch=%" PRIu64
                     ",lane=%u,seq=%" PRIu64 ",total=%" PRIu64
                     ",src=%u,demand=%" PRIu64 ",group=%" PRIu64
                     ",subgroup=%" PRIu64 ",eor=1",
                     r->epoch, r->lane, r->seq, r->total, r->src, r->demand,
                     r->group_id, r->subgroup_id);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

moqr_cli_blocked_kind_t
moqr_cli_seallog_parse(const char *line, moqr_cli_seallog_row_t *out)
{
    if (out == NULL) {
        return MOQR_BLOCKED_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    char work[MOQR_BLOCKED_ROW_MAX];
    char *t[9];
    if (!diag_tokens(line, MOQR_SEALLOG_PREFIX, work, sizeof(work), t, 9)) {
        return MOQR_BLOCKED_MALFORMED;
    }
    if (kv_u64(t[0], "epoch", &out->epoch) &&
        kv_u32(t[1], "lane", &out->lane) &&
        kv_u64(t[2], "seq", &out->seq) &&
        kv_u64(t[3], "total", &out->total) &&
        kv_u32(t[4], "src", &out->src) &&
        kv_u64(t[5], "demand", &out->demand) &&
        kv_u64(t[6], "group", &out->group_id) &&
        kv_u64(t[7], "subgroup", &out->subgroup_id)) {
        return MOQR_BLOCKED_ROW;
    }
    return MOQR_BLOCKED_MALFORMED;
}

#define FAIL(msg)                                                           \
    do {                                                                    \
        if (why != NULL) {                                                  \
            *why = (msg);                                                   \
        }                                                                   \
        return false;                                                       \
    } while (0)

bool
moqr_cli_sgdiag_accept(const moqr_cli_sgdiag_row_t *rows, size_t count,
                       uint64_t epoch, uint32_t lane, uint32_t *out_slot,
                       uint64_t *out_demand, const char **why)
{
    if (rows == NULL || count == 0 || epoch == 0) {
        FAIL("sgdiag: rows/count/epoch invalid");
    }
    size_t blocked = 0;
    uint32_t slot = 0;
    uint64_t demand = 0;
    uint32_t ambiguous = 0;
    for (size_t i = 0; i < count; i++) {
        if (rows[i].epoch != epoch) {
            FAIL("sgdiag: row epoch mismatch");
        }
        if (rows[i].lane != lane) {
            FAIL("sgdiag: wrong lane (someone else's evidence)");
        }
        if (!moqr_cli_sgdiag_row_consistent(&rows[i])) {
            FAIL("sgdiag: row inconsistent");
        }
        for (size_t j = 0; j < i; j++) {
            if (rows[j].slot == rows[i].slot) {
                FAIL("sgdiag: duplicate slot");
            }
        }
        if (rows[i].bind_sg_total > 0) {
            blocked++;
            slot = rows[i].slot;
            demand = rows[i].bind_sg_demand;
            ambiguous = rows[i].bind_sg_demand_ambiguous;
        }
    }
    if (blocked != 1) {
        FAIL("sgdiag: blocked target ambiguous (need exactly one "
             "bind_sg_total > 0 row)");
    }
    /* The blocked row must IDENTIFY its demand — the join to the SEALLOG
     * seals is exact or it is nothing. */
    if (demand == 0) {
        FAIL("sgdiag: blocked row carries no demand attribution");
    }
    if (ambiguous != 0) {
        FAIL("sgdiag: blocked row's demand attribution is AMBIGUOUS "
             "(two demands refused on the same conn)");
    }
    if (out_slot != NULL) {
        *out_slot = slot;
    }
    if (out_demand != NULL) {
        *out_demand = demand;
    }
    return true;
}

size_t
moqr_cli_seallog_count_demand(const moqr_cli_seallog_row_t *rows,
                              size_t count, uint64_t demand,
                              uint64_t since_seq)
{
    size_t n = 0;
    if (rows == NULL) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (rows[i].demand == demand && rows[i].seq >= since_seq) {
            n++;
        }
    }
    return n;
}

int
moqr_cli_seallog_meta_format(char *buf, size_t n,
                             const moqr_cli_seallog_meta_t *m)
{
    if (buf == NULL || m == NULL) {
        return -1;
    }
    int w = snprintf(buf, n,
                     MOQR_SEALMETA_PREFIX ",epoch=%" PRIu64
                     ",lane=%u,first_retained_seq=%" PRIu64
                     ",lifetime_total=%" PRIu64 ",retained_count=%" PRIu64
                     ",eor=1",
                     m->epoch, m->lane, m->first_retained_seq,
                     m->lifetime_total, m->retained_count);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

moqr_cli_blocked_kind_t
moqr_cli_seallog_meta_parse(const char *line, moqr_cli_seallog_meta_t *out)
{
    if (out == NULL) {
        return MOQR_BLOCKED_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    char work[MOQR_BLOCKED_ROW_MAX];
    char *t[6];
    if (!diag_tokens(line, MOQR_SEALMETA_PREFIX, work, sizeof(work), t, 6)) {
        return MOQR_BLOCKED_MALFORMED;
    }
    if (kv_u64(t[0], "epoch", &out->epoch) &&
        kv_u32(t[1], "lane", &out->lane) &&
        kv_u64(t[2], "first_retained_seq", &out->first_retained_seq) &&
        kv_u64(t[3], "lifetime_total", &out->lifetime_total) &&
        kv_u64(t[4], "retained_count", &out->retained_count)) {
        return MOQR_BLOCKED_ROW;
    }
    return MOQR_BLOCKED_MALFORMED;
}

bool
moqr_cli_seallog_accept_interval(const moqr_cli_seallog_meta_t *meta,
                                 const moqr_cli_seallog_row_t *rows,
                                 size_t count, uint64_t epoch, uint32_t lane,
                                 uint64_t previous_total, bool is_baseline,
                                 uint64_t *out_total, const char **why)
{
    if (meta == NULL) {
        FAIL("seallog: missing RELAY_SEALMETA header (no window evidence)");
    }
    if (epoch == 0) {
        FAIL("seallog: epoch invalid");
    }
    if (meta->epoch != epoch) {
        FAIL("seallog: meta epoch mismatch");
    }
    if (meta->lane != lane) {
        FAIL("seallog: meta wrong lane (someone else's evidence)");
    }
    if (meta->retained_count != (uint64_t)count) {
        FAIL("seallog: meta retained_count disagrees with row count");
    }
    if (meta->first_retained_seq > meta->lifetime_total) {
        FAIL("seallog: meta first_retained_seq past the lifetime total");
    }
    /* The window must be EXACTLY the contiguous interval
     * [first_retained_seq, lifetime_total): the right COUNT of rows... */
    if (meta->lifetime_total - meta->first_retained_seq != (uint64_t)count) {
        FAIL("seallog: retained window not contiguous with lifetime total "
             "(gap, overwrite, or truncated capture)");
    }
    /* ...and each of them the expected seq in ascending order. */
    for (size_t i = 0; i < count; i++) {
        if (rows == NULL) {
            FAIL("seallog: rows NULL but retained_count > 0");
        }
        if (rows[i].epoch != epoch) {
            FAIL("seallog: row epoch mismatch");
        }
        if (rows[i].lane != lane) {
            FAIL("seallog: row wrong lane (someone else's evidence)");
        }
        if (rows[i].total != meta->lifetime_total) {
            FAIL("seallog: row/meta disagree on the lifetime total");
        }
        if (rows[i].seq != meta->first_retained_seq + (uint64_t)i) {
            FAIL("seallog: seq gap or reorder inside the retained window");
        }
    }
    /* The FIRST epoch establishes the cursor; any self-consistent window is a
     * valid baseline and history before it need not have survived. */
    if (is_baseline) {
        if (out_total != NULL) {
            *out_total = meta->lifetime_total;
        }
        return true;
    }
    /* A later epoch is interval-complete iff the ring still covers the cursor.
     * A cursor ahead of the total is impossible; a cursor below the window's
     * lower edge means the interval since it was OVERWRITTEN. */
    if (previous_total > meta->lifetime_total) {
        FAIL("seallog: cursor ahead of the lifetime total (impossible)");
    }
    if (previous_total < meta->first_retained_seq) {
        FAIL("seallog: interval since the cursor was OVERWRITTEN by the ring");
    }
    /* [previous_total, lifetime_total) is a suffix of the contiguous window,
     * hence every event in it is present. */
    if (out_total != NULL) {
        *out_total = meta->lifetime_total;
    }
    return true;
}

moqr_sg_diagnosis_t
moqr_cli_sg_diagnose(bool seal_present, uint32_t occ, uint32_t cap,
                     uint32_t ready)
{
    if (ready > 1 || occ > cap) {
        return MOQR_SG_DIAG_INVALID;
    }
    if (!seal_present) {
        return MOQR_SG_DIAG_NOT_APPLIED;
    }
    if (occ == cap) {
        return ready ? MOQR_SG_DIAG_PASS_INCOMPLETE : MOQR_SG_DIAG_NO_RELEASE;
    }
    return ready ? MOQR_SG_DIAG_DOORBELL_UNCONSUMED : MOQR_SG_DIAG_NOT_REARMED;
}

const char *
moqr_cli_sg_diagnosis_str(moqr_sg_diagnosis_t d)
{
    switch (d) {
    case MOQR_SG_DIAG_NOT_APPLIED:
        return "expected seal absent from a complete interval: it did not "
               "reach destination seal application";
    case MOQR_SG_DIAG_NO_RELEASE:
        return "seal applied but no current bind readiness/slot release";
    case MOQR_SG_DIAG_PASS_INCOMPLETE:
        return "binding is ready but another delivery pass has not completed";
    case MOQR_SG_DIAG_NOT_REARMED:
        return "slot released but retained work was not reselected/rearmed";
    case MOQR_SG_DIAG_DOORBELL_UNCONSUMED:
        return "ready work exists but the pump/doorbell has not consumed it";
    case MOQR_SG_DIAG_INVALID:
    default:
        return "invalid observation (occ > cap or flag out of range)";
    }
}

bool
moqr_cli_blocked_accept(const moqr_cli_blocked_row_t *rows, size_t count,
                        uint32_t lanes, uint64_t epoch,
                        moqr_blocked_mode_t mode, const char **why)
{
    if (rows == NULL) {
        FAIL("rows is NULL");
    }
    if (lanes == 0 || epoch == 0) {
        FAIL("lanes and epoch must be nonzero");
    }
    if (count != (size_t)lanes) {
        FAIL("row count != lane count");
    }
    /* exactly lanes 0..lanes-1, ascending, each once, same epoch, consistent */
    for (uint32_t i = 0; i < lanes; i++) {
        const moqr_cli_blocked_row_t *r = &rows[i];
        if (r->lane != i) {
            FAIL("lanes not ascending 0..K-1 without gaps/dups");
        }
        if (r->epoch != epoch) {
            FAIL("row epoch mismatch (stale/mixed snapshot)");
        }
        if (!moqr_cli_blocked_row_consistent(r)) {
            FAIL("row cross-field inconsistency");
        }
    }
    const moqr_cli_blocked_row_t *l0 = &rows[0];
    /* The scenario blocks exactly ONE binding on lane 0 for the selected
     * reason. Lane 0 must show that reason and its current park; no other
     * lane may show the reason's count/total/park; and the park population
     * that the reason does NOT drive must be zero on EVERY lane (the two
     * parking reasons never co-occur in a single-blockage scenario, and
     * BIND_SG never parks at all). */
    switch (mode) {
    case MOQR_BLK_ACTION_CAP:
        if (l0->conns_action_cap != 1 || l0->action_cap_total < 1) {
            FAIL("action-cap: lane0 conns_action_cap!=1 or total<1");
        }
        if (l0->parked_action_cap != 1) {
            FAIL("action-cap: lane0 not currently parked (parked!=1)");
        }
        for (uint32_t i = 1; i < lanes; i++) {
            if (rows[i].conns_action_cap != 0 ||
                rows[i].action_cap_total != 0 ||
                rows[i].parked_action_cap != 0) {
                FAIL("action-cap: another lane reached the reason");
            }
        }
        for (uint32_t i = 0; i < lanes; i++) {
            if (rows[i].parked_session_sg != 0) {
                FAIL("action-cap: a lane parked on session-sg (must be 0)");
            }
        }
        break;
    case MOQR_BLK_SESSION_SG:
        if (l0->conns_session_sg != 1 || l0->session_sg_total < 1) {
            FAIL("session-sg: lane0 conns_session_sg!=1 or total<1");
        }
        if (l0->parked_session_sg != 1) {
            FAIL("session-sg: lane0 not currently parked (parked!=1)");
        }
        for (uint32_t i = 1; i < lanes; i++) {
            if (rows[i].conns_session_sg != 0 ||
                rows[i].session_sg_total != 0 ||
                rows[i].parked_session_sg != 0) {
                FAIL("session-sg: another lane reached the reason");
            }
        }
        for (uint32_t i = 0; i < lanes; i++) {
            if (rows[i].parked_action_cap != 0) {
                FAIL("session-sg: a lane parked on action-cap (must be 0)");
            }
        }
        break;
    case MOQR_BLK_BIND_SG:
        if (l0->conns_bind_sg != 1 || l0->bind_sg_total < 1) {
            FAIL("bind-sg: lane0 conns_bind_sg!=1 or total<1");
        }
        for (uint32_t i = 1; i < lanes; i++) {
            if (rows[i].conns_bind_sg != 0 || rows[i].bind_sg_total != 0) {
                FAIL("bind-sg: another lane reached the reason");
            }
        }
        for (uint32_t i = 0; i < lanes; i++) {
            if (rows[i].parked_action_cap != 0 ||
                rows[i].parked_session_sg != 0) {
                FAIL("bind-sg: a lane has a parked population (must be 0)");
            }
        }
        break;
    default:
        FAIL("unknown mode");
    }
    return true;
}
