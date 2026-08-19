#ifndef MOQR_OBS_H
#define MOQR_OBS_H

/*
 * Relay observability serializers.
 *
 * Pure renderers over stat snapshots: no I/O, no allocation, no clock. The
 * relay executable (or a test) snapshots core + binding stats on the network
 * thread, then renders here; the surface that actually writes bytes to a
 * socket/file/signal pipe lives in the CLI, never in this layer.
 *
 * Metric cardinality is bounded by construction: labels are closed-
 * vocabulary tokens (shard, transport, draft version, and per-series enum
 * reason/state) — never namespace, track, session, or cursor identity. Per-
 * entity detail lives in the route dump and the trace ring, not in metrics.
 */

#include <moqrelay/relay.h>

#include <moqr_bind.h>
#include <moqr_shards.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded labels stamped on every series. `transport` and `version` are
 * closed-set tokens owned by the CLI (e.g. "picoquic"/"draft-18"); NULL
 * renders as an empty value. */
typedef struct moqr_obs_labels {
    uint16_t    shard;
    const char *transport;
    const char *version;
} moqr_obs_labels_t;

/*
 * Render Prometheus text-format exposition for a snapshot of core (and,
 * when non-NULL, binding) stats into `buf`. Pure and deterministic for a
 * given snapshot. `*written` is the content length (excluding the NUL).
 * Returns MOQR_OK when content + NUL fit; MOQR_ERR_CAPACITY on truncation,
 * with `*written` set to the length the full output needs (grow to
 * *written + 1 and retry).
 */
moqr_result_t moqr_metrics_write_prometheus(const moqr_core_stats_t *core,
                                            const moqr_bind_stats_t *bind,
                                            const moqr_obs_labels_t *labels,
                                            char *buf, size_t cap,
                                            size_t *written);

/*
 * One shard's stat snapshot inside a multi-shard exposition: the core
 * snapshot (required), the binding and cross-shard-plane snapshots
 * (optional), the per-shard labels, and the CLI's own count of actual
 * lane_wake calls (the merged-mask number — distinct from the mask-level
 * wake_requests counters inside `shard`).
 */
typedef struct moqr_snapshot_view {
    const moqr_core_stats_t   *core;    /* required                        */
    const moqr_bind_stats_t   *bind;    /* optional                        */
    const moqr_shards_stats_t *shard;   /* optional (K>1 cross-shard plane) */
    moqr_obs_labels_t          labels;
    uint64_t                   lane_wakes;   /* rendered only with `shard` */
} moqr_snapshot_view_t;

/*
 * Render ONE Prometheus exposition over every shard's snapshot: HELP/TYPE
 * exactly once per metric, per-shard series under the existing names and
 * shard/transport/version labels, then process aggregates under separate
 * moqrelay_process_* names (counters and gauges sum, high-water marks take
 * the max, histogram buckets/count/sum sum). route_epoch and journal_epoch
 * are per-shard identities and are never aggregated.
 *
 * Internal-entity exclusion: when a view carries shard stats, the
 * user-facing gauges subtract like from like — subscriptions{state} minus
 * pump subscriptions of the SAME state, bindings minus the manager's
 * binding slots, namespace subscriptions minus the manager watcher — and
 * the internals surface separately (moqrelay_internal_bindings,
 * moqrelay_pump_subscriptions{state}). Any subtraction that would go
 * negative is a broken invariant: the whole exposition is suppressed
 * (MOQR_ERR_INVAL, nothing written) rather than floored or invented.
 *
 * Same truncation contract as the single-snapshot writer.
 */
moqr_result_t moqr_metrics_write_prometheus_multi(
    const moqr_snapshot_view_t *views, uint32_t count, char *buf, size_t cap,
    size_t *written);

#ifdef __cplusplus
}
#endif

#endif /* MOQR_OBS_H */
