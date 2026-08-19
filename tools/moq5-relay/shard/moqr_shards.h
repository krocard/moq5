#ifndef MOQR_SHARDS_H
#define MOQR_SHARDS_H

/*
 * The multi-shard runtime: owns K single-writer relay shards (each a
 * {moqr_core_t, moqr_bind_t, moqr_trace_t}) and steps them deterministically.
 *
 * A shard is one affinity domain; all of a session's state lives on its home
 * shard, and tracks are owned by the shard their objects arrive on. Crossing a
 * shard boundary is bounded directed channels + explicit copies only — never
 * shared mutable state, never cross-shard rcbuf refs.
 *
 * moqr_shards_step runs one deterministic round across all shards in six
 * phases: inbound drain (control mailboxes + demand-channel answers),
 * per-shard bind pump, dirty reconcile, outbound control enqueue, the demand
 * phase (liveness probe + forwarding), and the data phase (admitted-object
 * extraction). Remote-owner
 * demand round-trips to the owning shard over a directed FIFO demand channel
 * — never the coalescing announce mailbox. When admit_remote_demand is off
 * the owner path refuses every forwarded demand with NOT_SUPPORTED;
 * admit_remote_demand opts a runtime into owner-side admission, where the
 * owner subscribes a pump-sub on the requester's pseudo-binding, round-trips
 * the full ACK/DONE/cancel lifecycle, and pumps admitted records —
 * whole-object, chunked/live-edge, and the reset/eviction/seal terminals —
 * back over the same ordered channel, every crossing payload an
 * independently owned clone, never a shared rcbuf. The production CLI turns
 * admission ON automatically whenever it runs multiple lanes (a subscriber
 * on one lane must reach a publisher on another); a single-lane relay never
 * needs it. At K > 1 the
 * manager replicates announces across shards over the control mailboxes —
 * each shard mirrors the rendezvous winner of every namespace
 * onto a per-origin pseudo-binding, and a LOCAL publisher that loses the
 * rendezvous is force-withdrawn from its own shard (announce + sourced tracks
 * purged, subscribers terminated with GOING_AWAY, a publisher cancel queued) so
 * the winner's mirror can install. At K == 1 no manager entities are
 * built, so the runtime is a pure pass-through to a single moqr_bind_pump —
 * behaviour identical to a single-shard relay.
 *
 * Threading: a shard is single-caller — all of a shard's phases run from one
 * caller at a time (one deterministic loop in tests; one stepper per shard in
 * the eventual threaded runtime). Concurrent per-shard steppers meet in
 * exactly two places, the directed control mailboxes and the directed demand
 * channels, and each instance of either carries its own leaf mutex: the
 * producer (source shard) and the consumer (destination shard) serialize on
 * that one lock and nothing else. The deterministic runner uses the same code
 * path, uncontended. When shards step concurrently the configured allocator
 * must be thread-safe — mailbox and demand-channel message memory crosses the
 * boundary with the message (producer allocates, consumer frees), so
 * per-shard pool allocators are not usable for it.
 */

#include <moqrelay/relay.h>
#include <moqrelay/placement.h>
#include <moqrelay/trace.h>

#include <moqr_bind.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct moqr_shards moqr_shards_t;

/* Hard cap on shards (bounds the per-step visitation-order array; the 16-bit
 * handle shard tag allows far more, but this is plenty and keeps the step path
 * allocation-free). */
#define MOQR_SHARDS_MAX 64u

typedef struct moqr_shards_cfg {
    uint32_t           struct_size;
    const moq_alloc_t *alloc;              /* required; thread-safe when
                                              shards step concurrently        */
    uint16_t           shards;             /* K; 0 or 1 = single shard        */
    uint32_t           trace_ring_records; /* per-shard trace ring; 0 = def   */
    /* Placement policy (which shard owns a key). NULL = the built-in
     * rendezvous (highest-random-weight) hash over the local shard set. */
    moqr_place_fn      placement;
    /* Deterministic per-round shard visitation order: 0 = ascending; nonzero
     * seeds a permutation (schedule search in simulation). */
    uint64_t           permute_seed;
    /* Per-shard templates. Init both (moqr_shards_cfg_init_sized does so); the
     * runtime copies each per shard and overrides trace + shard_index/count on
     * the core, and core on the bind. */
    moqr_core_relay_cfg_t core_cfg;
    moqr_bind_cfg_t       bind_cfg;
    /* -- fields appended after the original core/bind templates (struct-size /
     * prefix discipline: new config always grows at the tail). -- */
    /* Per directed shard-pair control-mailbox capacity (namespaces in flight);
     * 0 = default. A full mailbox is cursor lag, never loss — the journal holds
     * the durable state and the sender re-exports when space frees. Tests set this
     * small to exercise the backpressure path. */
    uint32_t           mailbox_entries;
    /* Per-shard candidate-set journal capacity (namespaces tracked); 0 = default
     * (the core's max_ns_nodes). A shard's journal can exceed its trie nodes (it
     * also holds remote-only candidates), so this is an independent bound. When it
     * fills, inbound control messages stall in the mailbox (cursor lag) and a
     * borrowed local observation that cannot be recorded fail-stops the step. */
    uint32_t           journal_entries;
    /* Per-shard pending remote-owner demand capacity (upstream-subscribe attempts
     * awaiting refusal); 0 = default (the core's max_tracks — at most one upstream
     * attempt per track). A borrowed UPSTREAM_SUBSCRIBE that cannot be recorded
     * fail-stops the step. Tests set this small to exercise that path. */
    uint32_t           pending_demand_entries;
    /* Inbound control visibility, fixed at create. false (default): the
     * deterministic round barrier — a push made in round r is visible at r+1
     * under moqr_shards_step. true: a push is visible as soon as its mailbox
     * mutex publishes it, for a runtime that drives shards concurrently with
     * moqr_shards_step_shard and never advances a shared round. Set true ONLY
     * when each shard is stepped by its own caller; never mix with
     * moqr_shards_step. */
    bool               live_visibility;
    /* Per directed shard-pair demand-channel ENTRY capacity (in-flight
     * messages); 0 = default (the pending-demand cap — CONTROL keeps at most
     * one in-flight message per demand per direction; queued data objects
     * are additionally bounded by the byte cap below). A full channel is
     * backpressure on the sender, never loss. */
    uint32_t           demand_channel_entries;
    /* Owner-side demand admission, fixed at create. false (default): the
     * owner refuses every forwarded demand with NOT_SUPPORTED. true: the
     * owner admits a demand as an ordinary subscribe on the requester's
     * pseudo-binding, round-trips the full ACK/DONE lifecycle (accept,
     * terminal codes verbatim, staged cancel), and pumps admitted data —
     * whole-object, chunked/live-edge, and the reset/eviction/seal
     * terminals — back over the demand channel. The production CLI sets
     * this true whenever it runs multiple lanes; a single-lane (K == 1)
     * relay leaves it false and builds no manager, so the field is inert
     * there regardless. */
    bool               admit_remote_demand;
    /* Per directed shard-pair demand-channel LOGICAL-BYTE cap: the sum of
     * queued payload bytes + properties bytes + control canonical-key bytes
     * a channel may hold. Data pushes are gated on it (control is counted
     * but exempt — it carries no payload and must not starve). 0 = default:
     * the resolved per-track log byte budget, so any single legal record —
     * which the log bounds to that budget — is always eventually sendable.
     * An explicit value SMALLER than that resolved bound could strand a
     * legal record and is refused at create (MOQR_ERR_INVAL). This is a
     * per-channel bound, not a process-wide ceiling. */
    uint64_t           demand_channel_bytes;
    /* Per-shard data-pump turn budgets (one turn = one shard step's data
     * phase): messages per turn (0 = 64) and logical bytes per turn (0 =
     * the channel byte cap / 4). The first data message of an otherwise
     * empty turn may exceed the byte budget — a legal message up to the
     * channel cap always makes progress — after which the turn ends; no
     * budget debt carries into the next turn. */
    uint32_t           pump_turn_messages;
    uint64_t           pump_turn_bytes;
    /* Per-demand subgroup-progress capacity: how many DISTINCT live
     * (group, subgroup) streams one admitted demand may have in flight, on
     * each side of the boundary (the owner's chunk-resume cursors and the
     * requester's open-object bookkeeping). A slot lives for the subgroup's
     * lifetime — completed objects reuse it — so this bounds concurrent
     * subgroups, not objects. Exhaustion is a loss-visible demand terminal
     * (INTERNAL), never a dropped object. 0 = default: the session binding's
     * resolved max_open_subgroups (its default when the template leaves it
     * unset). */
    uint32_t           pump_subgroup_slots;
} moqr_shards_cfg_t;

/* The pure, prefix-safe resolution moqr_shards_create applies to a config —
 * shared by create and the capacity describe so the two can never disagree.
 * Fails INVAL exactly where create would: an explicit demand_channel_bytes
 * below one resolved log record, an admission progress-table product the
 * index arithmetic cannot represent, or (K>1) a core max_bindings that
 * cannot fit the K manager-owned binding slots plus at least one client. */
typedef struct moqr_shards_limits {
    uint16_t shards;
    bool     admit;
    uint32_t mbox_cap;
    uint32_t jrn_cap;
    uint32_t pend_cap;
    uint32_t dch_cap;
    uint64_t dch_byte_cap;
    uint32_t sg_slots;
    uint32_t trace_ring;
    uint32_t pump_turn_msgs;
    uint64_t pump_turn_bytes;
    uint32_t usable_bindings;   /* min(resolved bind max_conns,
                                   K==1 ? max_bindings : max_bindings-K) */
} moqr_shards_limits_t;

moqr_result_t moqr_shards_cfg_resolve(const moqr_shards_cfg_t *cfg,
                                      moqr_shards_limits_t *out);

/* The relay-state allocation-request ceiling for a shard runtime built from
 * this config: every byte of allocation REQUEST backing relay-owned state —
 * persistent structures, logical retained payload bytes, rcbuf headers
 * behind retained/coalesced/in-flight content, canonical-key pools, and
 * allocate-before-push staging. Models allocator requests only: malloc
 * metadata, size-class rounding, fragmentation, thread stacks, transient
 * diagnostic buffers, and memory that remains adapter/session-owned are
 * outside it. Pure; checked arithmetic (overflow = INVAL); at K == 1 every
 * cross-shard term is zero and admission adds nothing (structurally inert).
 */
typedef struct moqr_shards_capacity {
    uint64_t core_structure_bytes;   /* per shard                          */
    uint64_t core_payload_bytes;     /* per shard                          */
    uint64_t bind_structure_bytes;   /* per shard (incl. announce bytes)   */
    uint64_t trace_bytes;            /* per shard                          */
    uint64_t per_shard_bytes;
    uint64_t shards_structure_bytes; /* runtime container + K>1 pools      */
    uint64_t channel_byte_ceiling;   /* K^2 x (data cap + control-exempt
                                        canon + clone headers)             */
    uint64_t canon_byte_ceiling;     /* mailbox + journal + pending keys   */
    uint64_t staging_byte_ceiling;   /* allocate-before-push, one/shard    */
    uint64_t relay_alloc_ceiling;    /* total of the above                 */
    uint32_t usable_bindings_per_shard;
    uint16_t shards;
    bool     admission;
} moqr_shards_capacity_t;

moqr_result_t moqr_shards_capacity_describe(const moqr_shards_cfg_t *cfg,
                                            moqr_shards_capacity_t *out);

/* Zero + set struct_size and alloc, and init the nested core/bind templates
 * with the same alloc. The caller then sets shards + per-shard budgets. */
void moqr_shards_cfg_init_sized(moqr_shards_cfg_t *cfg, size_t cfg_size,
                                const moq_alloc_t *alloc);

moqr_result_t moqr_shards_create(const moqr_shards_cfg_t *cfg,
                                 moqr_shards_t **out);
void moqr_shards_destroy(moqr_shards_t *s);

/* One deterministic step across every shard: six phases per shard, shards
 * visited in the seeded order. now_us is the round's clock (one value fed to
 * every shard). Returns the first non-OK per-shard pump result, else MOQR_OK
 * (so at K == 1 it returns exactly what moqr_bind_pump returned). */
moqr_result_t moqr_shards_step(moqr_shards_t *s, uint64_t now_us);

uint16_t      moqr_shards_count(const moqr_shards_t *s);
moqr_core_t  *moqr_shards_core(moqr_shards_t *s, uint16_t shard);
moqr_bind_t  *moqr_shards_bind(moqr_shards_t *s, uint16_t shard);
moqr_trace_t *moqr_shards_trace(moqr_shards_t *s, uint16_t shard);

/* Which shard owns `key` under the current placement state. Pure over
 * (key, state); the same key returns the same owner every call/replay. */
moqr_owner_t  moqr_shards_place(const moqr_shards_t *s,
                                const moqr_place_key_t *key);

/* Step ONE shard's six phases (no round advance) — the entry a concurrent
 * runtime calls, one caller per shard. `pushed_dst_mask` (optional) is zeroed,
 * then reports the WAKE SET for this step: bit d for every destination shard
 * whose control mailbox or demand channel accepted an outbound push, PLUS bit
 * p for every producer shard whose p->this demand channel regained capacity
 * through a durable pop (the producer credit — a lane held on channel-full
 * must never wait for an idle sweep), PLUS this shard's OWN bit when the step
 * left local work no external event re-signals (a pump pass that ended on its
 * budget, or a phase that queued core intents after the pass that would have
 * drained them). A caller must accept the self bit and step this shard again;
 * one such continuation per unfinished step, never a standing request.
 * Redundant wakes are possible; lost credit wakes are not. Returns the bind
 * pump's result; if that is OK but this shard's manager has latched fail-stop
 * (a borrowed observation was lost — sticky), returns MOQR_ERR_NOMEM. Pair
 * with live_visibility = true; never interleave with moqr_shards_step (which
 * owns the round barrier and the cross-shard oom scan). */
moqr_result_t moqr_shards_step_shard(moqr_shards_t *s, uint16_t shard,
                                     uint64_t now_us,
                                     uint64_t *pushed_dst_mask);

/* -- production observability ------------------------------------------------ */

/*
 * The cross-shard demand-channel message vocabulary, as a bounded public
 * index space for the per-kind enqueue counters below (and their metric /
 * benchmark labels). Append-only, never renumbered; the implementation
 * static-asserts each value against its wire-side counterpart, so a drift
 * breaks the build, not the accounting.
 */
typedef uint32_t moqr_shards_msg_kind_t;

#define MOQR_SHARDS_MSG_DEMAND     0u
#define MOQR_SHARDS_MSG_UNDEMAND   1u
#define MOQR_SHARDS_MSG_DONE       2u
#define MOQR_SHARDS_MSG_ACK        3u
#define MOQR_SHARDS_MSG_OBJ        4u
#define MOQR_SHARDS_MSG_OBJ_OPEN   5u
#define MOQR_SHARDS_MSG_OBJ_CHUNK  6u
#define MOQR_SHARDS_MSG_OBJ_END    7u
#define MOQR_SHARDS_MSG_OBJ_RESET  8u
#define MOQR_SHARDS_MSG_GRP_RESET  9u
#define MOQR_SHARDS_MSG_GRP_EVICT 10u
#define MOQR_SHARDS_MSG_SG_SEAL   11u
#define MOQR_SHARDS_MSG__COUNT    12u

/* Stable lower-snake label token for a message kind (e.g. "obj_open");
 * out-of-range values render "unknown". */
const char *moqr_shards_msg_kind_name(moqr_shards_msg_kind_t kind);

/*
 * One shard's cross-shard-plane statistics. Every counter/gauge here is the
 * shard-manager plane only — the per-shard core and binding keep their own
 * stats (moqr_core_get_stats / moqr_bind_get_stats). At K == 1, and for the
 * admission-gated fields with admission off, everything is structurally
 * zero.
 */
typedef struct moqr_shards_stats {
    /* -- counters (monotonic) -- */
    uint64_t pump_turns;        /* data phases that attempted extraction    */
    uint64_t pump_messages;     /* data messages pushed (phase 6 only)      */
    uint64_t pump_bytes;        /* logical bytes pushed by the data pump    */
    /* Successful DURABLE demand-channel enqueues by this shard as the
     * PRODUCER, indexed by moqr_shards_msg_kind_t. An attempt refused by a
     * full channel, the byte gate, or the arbiter is never counted. */
    uint64_t enqueued[MOQR_SHARDS_MSG__COUNT];
    uint64_t remote_demand_refused;   /* requester-resolved owner refusals  */
    uint64_t remote_demand_resolved;  /* owner DONEs resolved as requester  */
    uint64_t remote_data_rejected;    /* ingest refused TOO_OLD (loss)      */
    uint64_t term_capacity;           /* demand terminals: capacity edge    */
    uint64_t term_overrun;            /* demand terminals: progress table   */
    /* Wake requests raised by this shard's steps, counted at the MASK
     * level by BOTH runners (moqr_shards_step and moqr_shards_step_shard):
     * every step adds popcount(push mask) and popcount(credit mask) once —
     * several messages toward one destination in one step are ONE wake
     * request. The CLI's own counter of actual lane_wake calls (the merged
     * mask) is a separate, CLI-owned number. */
    uint64_t wake_requests_push;
    uint64_t wake_requests_credit;
    /* The LOCAL continuation cause, distinct from both above, from two exact
     * outcomes: (a) this step applied inbound DATA into its own core
     * (ingest/seal/reset/evict) AND its one bind-pump pass left RESIDUAL ready
     * work (a mark that landed during the pass, which nothing else re-signals),
     * or (b) that pass ended on the delivery-budget guard with work remaining.
     * Either way the step requests exactly ONE coalesced self-wake. Never
     * counted as a push (nothing crossed toward another shard) or a credit (no
     * producer regained room). A step that applies inbound data but whose pass
     * DRAINS everything requests nothing; conversely (b) can request one even
     * when nothing was applied. */
    uint64_t wake_requests_local;
    /* Turn-outcome classification: the four turns_* counters PARTITION
     * pump_turns exactly (one increment per data-phase turn, decided at a
     * single classification point with pre-registered precedence: message
     * budget > byte budget > blocked > drained — the tie between message
     * and byte exhaustion is never data-dependent). turns_with_messages
     * counts turns that pushed at least one data message, independent of
     * class; arb_class_refusals counts sticky-arbiter class-gate refusals
     * raised by this shard's own pushes. */
    uint64_t turns_msg_budget;
    uint64_t turns_byte_budget;
    uint64_t turns_blocked;
    uint64_t turns_drained;
    uint64_t turns_with_messages;
    uint64_t arb_class_refusals;
    /* -- gauges (current) -- */
    uint32_t pending_demands;         /* requester-side recorded demands    */
    uint32_t pump_subs_parked;        /* owner pump-subs by core sub state  */
    uint32_t pump_subs_active;        /*   (a stale/retired sub is GONE and */
                                      /*    appears in NEITHER state)       */
    uint32_t owner_progress_slots;    /* live owner chunk-resume cursors    */
    uint32_t requester_open_objects;  /* live requester open-object slots   */
    /* Manager-owned entities inside the shard core's own gauges: K binding
     * slots (K-1 origin pseudo-bindings + the watcher binding) and one
     * wildcard namespace subscription. The exporter subtracts these from
     * the core's user-facing gauges and surfaces them separately. */
    uint32_t internal_bindings;
    uint32_t internal_ns_subs;
    uint32_t mailbox_pending;         /* inbound control slots, all sources */
    uint64_t inbound_channel_entries; /* inbound demand-channel occupancy   */
    uint64_t inbound_channel_bytes;   /*   (sum over every src -> this)     */
    /* The manager-local journal projection generation: bumped on every
     * actual change to the printed journal projection (candidate set,
     * winner, mirror, holdout, entry reclaim) and never on retries,
     * no-ops, or self-echoes. 0 at K == 1. */
    uint64_t journal_epoch;
    /* -- high-water marks -- */
    /* Per-directed-channel maxima, tracked under each channel's own leaf
     * mutex; reported as the maximum over this shard's INBOUND channels. */
    uint32_t channel_entries_hwm;
    uint64_t channel_bytes_hwm;
} moqr_shards_stats_t;

/*
 * Snapshot one shard's cross-shard-plane stats. Threading: call ONLY from
 * within that shard's owning LANE (the serialized lock domain stepping it
 * — which may migrate across OS threads, so this is a serialization
 * contract, not thread affinity), or from any caller after every lane has
 * joined — it reads manager state the owning stepper mutates lock-free,
 * plus channel/mailbox occupancy under their own leaf mutexes (never a
 * global lock, never another shard's core). MOQR_ERR_INVAL on a bad shard
 * index; MOQR_ERR_INVAL (all-zero out) when a LIVE pump-sub reports an
 * impossible state — only a stale handle maps to "gone"; at K == 1 every
 * field is zero.
 */
moqr_result_t moqr_shards_get_stats(moqr_shards_t *s, uint16_t shard,
                                    moqr_shards_stats_t *out);

/*
 * Per-directed-pair cumulative accounting for the src -> dst demand
 * channel. Data and control are counted separately because they close
 * against different oracles: summed over every dst, data_messages ==
 * that producer's pump_messages and data_bytes == pump_bytes (the same
 * logical-byte currency), while control_messages sums to the control
 * kinds of its enqueued[] vector. The refusal split records THE
 * predicate that fired (entries checked first, matching the admission
 * order; one increment per refusal event). Fields are maintained by the
 * producer inside the push critical section and snapshotted here under
 * the channel's own leaf mutex — same ownership contract as
 * moqr_shards_get_stats (owning lane, or post-join); MOQR_ERR_INVAL on a
 * bad index, a self pair (src == dst), or a NULL out.
 */
typedef struct moqr_shards_pair_stats {
    uint64_t data_messages;
    uint64_t data_bytes;
    uint64_t control_messages;
    uint64_t refused_entries;
    uint64_t refused_bytes;
} moqr_shards_pair_stats_t;

/* Frozen v0 layout floor: the smallest out_size this accessor accepts. */
#define MOQR_SHARDS_PAIR_STATS_V0_SIZE (5u * sizeof(uint64_t))

moqr_result_t moqr_shards_get_pair_stats(moqr_shards_t *s, uint16_t src,
                                         uint16_t dst,
                                         moqr_shards_pair_stats_t *out,
                                         size_t out_size);

/*
 * Render shard <i>'s manager journal as text: a `shard <i>` header, the
 * shard-local `journal_epoch=<j>`, then one line per entry in ascending
 * canonical-key order with the candidate bitset, HRW winner, installed
 * mirror, and the holdout flag. Namespace parts render LENGTH-DELIMITED
 * and binary-safe ([<len>]"..." with non-printable bytes escaped \xNN) —
 * canonical bytes are never treated as C strings and never truncated to a
 * prefix; output size is bounded by the canonical-key ceiling and the
 * journal capacity. Same ownership rule as moqr_shards_get_stats (owning
 * lane, or post-join) and the same truncation contract as the other
 * serializers: MOQR_OK when content + NUL fit, else MOQR_ERR_CAPACITY with
 * *written set to the full length needed. At K == 1 it renders the header
 * with journal_epoch=0 and no entries.
 */
moqr_result_t moqr_shards_journal_dump_text(moqr_shards_t *s, uint16_t shard,
                                            char *buf, size_t cap,
                                            size_t *written);

/* -- test/debug introspection of the cross-shard control plane -------------- */

typedef struct moqr_shards_jinfo {
    bool     present;      /* the shard's journal has an entry for this ns */
    uint64_t candidates;   /* live origin bitset (bit i = shard i announces it) */
    int32_t  winner;       /* HRW winner over the candidates, or -1 */
    int32_t  mirror;       /* installed PB-mirror origin, or -1 (none) */
    bool     holdout;      /* a local loser whose force-withdrawal is deferred
                            * (WOULD_BLOCK); clears once the withdrawal lands */
} moqr_shards_jinfo_t;

/* Read a shard manager's candidate-set journal entry for a namespace. `present`
 * is false at K == 1 (no manager) or when the namespace is absent. */
void moqr_shards_debug_journal(moqr_shards_t *s, uint16_t shard,
                               const moq_bytes_t *parts, uint32_t count,
                               moqr_shards_jinfo_t *out);

/* The HRW winner over a candidate bitset for a namespace (pure) — lets a test
 * pick a namespace a particular shard owns. -1 when no candidates. */
int32_t moqr_shards_debug_hrw_winner(uint64_t candidates,
                                     const moq_bytes_t *parts, uint32_t count);

/* Count of self-echoes a shard's manager still expects from its own mirror ops
 * (0 at K == 1). At a converged fixed point this is 0 — every mirror
 * announce/unannounce echo has been recognised and consumed, so a nonzero
 * steady-state value means a mirror echo was misclassified as a real event (or
 * vice-versa). */
uint32_t moqr_shards_debug_pending_tokens(moqr_shards_t *s, uint16_t shard);

/* Occupied slots in the directed src→dst control mailbox (0 at K == 1). A
 * quiesced runtime has none; a value that never drains signals a stuck mailbox. */
uint32_t moqr_shards_debug_mailbox_pending(moqr_shards_t *s, uint16_t src,
                                           uint16_t dst);

/* Count of remote-owner subscribe demands resolved as refused (0 at K == 1).
 * Counted once per attempt, WHEN THE REQUESTER RESOLVES THE OWNER'S ANSWER —
 * a demand refused locally (no live mirror) or cancelled in flight does not
 * count, so a nonzero value proves the round-trip completed. */
uint64_t moqr_shards_debug_remote_demand_refused(moqr_shards_t *s, uint16_t shard);

/* Remote-owner demands recorded but not yet resolved (any state: recorded,
 * forwarded, acknowledged-active, or awaiting the cancel notice). 0 at
 * K == 1. An acknowledged demand stays counted for as long as its track is
 * ACTIVE — the entry is the cancel notice's durable home. */
uint32_t moqr_shards_debug_pending_demand(moqr_shards_t *s, uint16_t shard);

/* Count of owner DONE answers this shard resolved into its own core as the
 * REQUESTER (pre-ACK refusals and post-ACK terminals both), with the last
 * resolution's error code (verbatim from the owner) and whether it was
 * pre-ACK. 0 at K == 1. */
uint64_t moqr_shards_debug_remote_demand_resolved(moqr_shards_t *s,
                                                  uint16_t shard,
                                                  uint64_t *last_code,
                                                  bool *last_pre_ack);

/* The complete tagged PUBLISH_DONE descriptor of the last owner DONE this
 * shard resolved as the REQUESTER — tag, 64-bit value and origin profile, not
 * just the wire number that `..._resolved` reports. Returns false, leaving
 * *out untouched, until this shard has resolved at least one DONE, and for
 * invalid inputs. A pre-ACK refusal resolves with NONE: that path is
 * REQUEST_ERROR-domain and states no terminal. */
bool moqr_shards_debug_remote_demand_last_pd(moqr_shards_t *s, uint16_t shard,
                                             moqr_pd_desc_t *out);

/* The tagged terminal carried by the head message of the directed src->dst
 * demand channel, read WITHOUT consuming it — what a re-peek sees. false when
 * the head is absent or is not a DONE. */
bool moqr_shards_debug_demand_channel_head_pd(moqr_shards_t *s, uint16_t src,
                                              uint16_t dst,
                                              moqr_pd_desc_t *out);

/* Admitted remote demands live on this shard as the OWNER (pump-subs whose
 * lifecycle is still open). 0 at K == 1, with admission off, and at any
 * quiesced fixed point. */
uint32_t moqr_shards_debug_owner_pump_subs(moqr_shards_t *s, uint16_t shard);

/* Queued LOGICAL bytes in the directed src→dst demand channel (payload +
 * properties + control canonical keys; 0 at K == 1). A quiesced runtime has
 * none. */
uint64_t moqr_shards_debug_demand_channel_bytes(moqr_shards_t *s,
                                                uint16_t src, uint16_t dst);

/* Data-pump counters for one shard as the OWNER: turns (data phases that
 * attempted extraction), messages pushed, and logical bytes pushed. All 0 at
 * K == 1 and with admission off. */
void moqr_shards_debug_pump_counters(moqr_shards_t *s, uint16_t shard,
                                     uint64_t *turns, uint64_t *messages,
                                     uint64_t *bytes);

/* Data records this shard as the REQUESTER refused at ingest for its own
 * retention horizon (TOO_OLD): counted, never a hidden drop. */
uint64_t moqr_shards_debug_remote_data_rejected(moqr_shards_t *s,
                                                uint16_t shard);

/* Remote demands this shard terminated for a local capacity edge (the
 * demand-terminal path; wire code INTERNAL 0x0). */
uint64_t moqr_shards_debug_remote_demand_term_capacity(moqr_shards_t *s,
                                                       uint16_t shard);

/* Remote demands this shard terminated because a subgroup-progress table
 * filled (distinct live subgroups beyond pump_subgroup_slots; INTERNAL 0x0,
 * distinguished from ingest-capacity terminals by this metric only). */
uint64_t moqr_shards_debug_remote_demand_term_overrun(moqr_shards_t *s,
                                                      uint16_t shard);

/* Live subgroup-progress slots on this shard: as the OWNER (chunk-resume
 * cursors across its admitted demands) and as the REQUESTER (open-object
 * bookkeeping across its pending demands). Both 0 at K == 1, with admission
 * off, and at any quiesced fixed point with no live streams. */
uint32_t moqr_shards_debug_owner_progress_slots(moqr_shards_t *s,
                                                uint16_t shard);
uint32_t moqr_shards_debug_requester_open_objects(moqr_shards_t *s,
                                                  uint16_t shard);

/* Occupied slots in the directed src→dst DEMAND channel (0 at K == 1). A
 * quiesced runtime has none. */
uint32_t moqr_shards_debug_demand_channel_pending(moqr_shards_t *s,
                                                  uint16_t src, uint16_t dst);

/* The {track, *track_gen} of pending-demand entry `idx` on this shard's manager
 * (idx < pending_demand). For tests that drive the recorded track out of PENDING
 * to exercise the refuse drain's stale / wrong-state moot handling. Returns
 * MOQR_TRACK_INVALID when out of range. */
moqr_track_t moqr_shards_debug_pending_demand_track(moqr_shards_t *s,
                                                    uint16_t shard, uint32_t idx,
                                                    uint64_t *track_gen);

/* Test alias for moqr_shards_step_shard (same contract, incl. the sticky
 * fail-stop → MOQR_ERR_NOMEM). Kept so existing tests that compose hand-rolled
 * passes with debug_round_advance keep reading clearly as test scaffolding. */
moqr_result_t moqr_shards_debug_step_shard(moqr_shards_t *s, uint16_t shard,
                                           uint64_t now_us,
                                           uint64_t *pushed_dst_mask);

/* Advance the round barrier exactly once, after every shard has stepped:
 * pushes stamped this round become visible next round. */
void moqr_shards_debug_round_advance(moqr_shards_t *s);

/* Inbound visibility policy. false (default): the deterministic round barrier
 * (pushed in round r ⇒ visible at r+1 under moqr_shards_step). true: a push is
 * visible as soon as the mailbox mutex publishes it — for tests free-running
 * per-shard steppers on their own threads, where no shared round exists (the
 * round counter is then never advanced, so it is never written concurrently).
 * Toggle only while quiesced: no stepper may be running. */
void moqr_shards_debug_set_live_visibility(moqr_shards_t *s, bool live);

#ifdef __cplusplus
}
#endif

#ifdef MOQR_BIND_TESTING
/* Test-only: latch this shard's manager fail-stop exactly as an unrecordable
 * borrowed observation does — sticky, never cleared — so a test can activate
 * the step-failure branch deterministically, with no traffic and no capacity
 * squeeze. No-op when the shard has no manager (K == 1). */
void moqr_shards_debug_fail_stop(moqr_shards_t *s, uint16_t shard);

/* Verify-only SG_SEAL ingest evidence (see moqr_shards.c seal_log): one
 * entry per seal this REQUESTER shard applied to its core, in ingest order. */
typedef struct moqr_shards_seal_ev {
    uint64_t seq;        /* lifetime ingest order on this shard */
    uint16_t src;        /* owner lane the seal crossed from    */
    uint64_t demand_id;  /* exact demand attribution            */
    uint64_t group_id;
    uint64_t subgroup_id;
} moqr_shards_seal_ev_t;

/* Copies the newest window, oldest first; *out_total (nullable) receives the
 * LIFETIME ingest count — total > returned means the ring overwrote evidence
 * (the fail-closed acceptance rejects such a record). */
uint32_t moqr_shards_debug_seal_log(const moqr_shards_t *s, uint16_t shard,
                                    moqr_shards_seal_ev_t *out, uint32_t cap,
                                    uint64_t *out_total);

/* Verify-only: resolve a REQUESTER-side track (raw handle + generation, as
 * captured by moqr_bind_debug_conn_bind_sg_track at a BIND_SG refusal) to
 * the demand id feeding it, by the shard's own pending-demand table — the
 * table is keyed by track, which is why the track is the join key between
 * the blocked conn and the SEALLOG's demand attribution. Returns 0 when the
 * shard is bad or no live demand matches (a purely LOCAL track has no
 * demand — the cross-shard acceptance fails closed on 0). */
uint64_t moqr_shards_debug_track_demand(const moqr_shards_t *s,
                                        uint16_t shard, uint64_t track_raw,
                                        uint64_t track_gen);

/* Verify-only invariant-breaker pair for the duplicate-resolution pin:
 * _pend_dup clones the pending-demand entry for `demand_id` under
 * `new_demand_id` (own canon copy, no shared progress row — false on
 * missing source / id collision / full table / OOM); _pend_drop retires the
 * entry for `demand_id` again. Together they prove the resolver above
 * returns 0 on a duplicate {track, gen} instead of silently choosing. */
bool moqr_shards_debug_pend_dup(moqr_shards_t *s, uint16_t shard,
                                uint64_t demand_id, uint64_t new_demand_id);
bool moqr_shards_debug_pend_drop(moqr_shards_t *s, uint16_t shard,
                                 uint64_t demand_id);
#endif

#endif /* MOQR_SHARDS_H */
