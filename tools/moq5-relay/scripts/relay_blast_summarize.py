#!/usr/bin/env python3
"""Summarize gauntlet relay-blast JSON reports (median/min/max per group).

Evidence-integrity rules, all hard gates:

* measurement basis — every report must carry metrics.measurement_basis ==
  "relay_e2e_wall"; any other class is an error, never averaged in.
* pass/fail separation — CANONICAL throughput/latency aggregates use
  correctness-passed rows ONLY. The blast driver publishes wall metrics even on
  a correctness failure, computed only over the objects actually received — a
  failed row's "rate" is a censored partial-progress diagnostic, not completed-
  work throughput, and its latency percentiles exclude the missing objects.
  A group with zero passing reps reports canonical values as `none`. Failed
  rows are summarized SEPARATELY under explicit diagnostic labels (delivery
  fraction, partial-progress rate, received-only p99).
* replication policy — with --expect-reps N (the orchestrator passes its
  configured repetition count), a group whose COMPLETED report count is below N
  emits NO canonical performance: reps lost to outer timeouts or launcher
  failures must not leave a survivor-only "median" (survivorship bias). The
  attempted count is always reported alongside the completed count.
* workload fingerprint — rows group by (impl label, semantic workload
  fingerprint: retrieval mode, alpn, topology counts, object shape/counts/
  sizes). Reports from different scenarios/ALPNs/retrieval modes/knobs can
  never silently pool into one median. Retrieval currently reaches the report
  only via an ergonomics note ("retrieval=subscribe|fetch ..."); exactly one
  such note is required — missing or conflicting retrieval metadata is a hard
  error (the gauntlet extension will make it a first-class field).
* strict fields — correctness.passed must be a JSON boolean; impl/alpn must be
  strings; every topology/size field must be a nonnegative integer; delivery
  counts must be nonnegative integers; rates/latencies must be finite
  nonnegative numbers; a workload whose expected delivery total is zero is
  rejected. Malformed JSON, missing fields, and unknown flags are hard errors.
"""

import json
import math
import sys

REQUIRED_BASIS = "relay_e2e_wall"

FP_COUNT_FIELDS = (
    "namespace_count", "tracks_per_namespace", "publishers_per_namespace",
    "subscribers_per_track", "groups", "objects_per_group",
    "first_object_size", "other_object_size",
)


def die(msg):
    sys.stderr.write("relay_blast_summarize: %s\n" % msg)
    sys.exit(2)


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    if n % 2:
        return float(s[n // 2])
    return (s[n // 2 - 1] + s[n // 2]) / 2.0


def _req_bool(d, key, path):
    v = d.get(key)
    if not isinstance(v, bool):
        die("report %s: '%s' must be a JSON boolean (got %r)" % (path, key, v))
    return v


def _req_str(d, key, path):
    v = d.get(key)
    if not isinstance(v, str) or v == "":
        die("report %s: '%s' must be a nonempty string (got %r)" %
            (path, key, v))
    return v


def _req_count(d, key, path):
    v = d.get(key)
    if isinstance(v, bool) or not isinstance(v, int) or v < 0:
        die("report %s: '%s' must be a nonnegative integer (got %r)" %
            (path, key, v))
    return v


def _req_num(d, key, path):
    v = d.get(key)
    if isinstance(v, bool) or not isinstance(v, (int, float)) or \
            not math.isfinite(float(v)) or float(v) < 0.0:
        die("report %s: '%s' must be a finite nonnegative number (got %r)" %
            (path, key, v))
    return float(v)


def _retrieval_mode(d, path):
    """Exactly one 'retrieval=<mode>' ergonomics note; missing/conflicting
    retrieval metadata is ambiguous and rejected (SUBSCRIBE and FETCH measure
    fundamentally different windows and must never pool)."""
    notes = d.get("ergonomics_notes")
    if not isinstance(notes, list):
        die("report %s: ergonomics_notes missing (need retrieval metadata)" %
            path)
    # Count MATCHING NOTES, not distinct modes: two identical retrieval notes
    # are just as ambiguous about provenance as two conflicting ones.
    found = [n.split("=", 1)[1].split(" ", 1)[0].split("(", 1)[0]
             for n in notes
             if isinstance(n, str) and n.startswith("retrieval=")]
    if len(found) != 1:
        die("report %s: %d 'retrieval=<mode>' notes (need exactly one)%s" %
            (path, len(found),
             "" if not found else ": %r" % sorted(found)))
    mode = found[0]
    if mode not in ("subscribe", "fetch"):
        die("report %s: unknown retrieval mode '%s'" % (path, mode))
    return mode


LANE_STATS_PREFIX = "RELAY_LANE_STATS_V1"    # legacy (historical logs)
LANE_STATS_PREFIX_V2 = "RELAY_LANE_STATS_V2"  # legacy (historical logs)
LANE_STATS_PREFIX_V3 = "RELAY_LANE_STATS_V3"
PAIR_STATS_PREFIX = "RELAY_PAIR_STATS_V1"
RUN_CONFIG_PREFIX = "RELAY_RUN_CONFIG_V1"
TURN_OUTCOME_KEYS = (
    "turns_msg_budget", "turns_byte_budget", "turns_blocked",
    "turns_drained", "turns_with_messages", "arb_class_refusals",
)
# V3 appends the third wake-request cause: the step-local continuation a
# lane requests of ITSELF after applying inbound data or ending a delivery
# pass on the budget guard (never pooled with push or credit).
LOCAL_WAKE_KEYS = ("wake_requests_local",)
PAIR_STATS_KEYS = (
    "data_messages", "data_bytes", "control_messages",
    "refused_entries", "refused_bytes",
)
RUN_CONFIG_KEYS = (
    "pump_turn_messages", "pump_turn_bytes",
    "demand_channel_entries", "demand_channel_bytes",
)

LANE_STATS_KEYS = (
    "wakes_same_lane", "wakes_cross_lane", "wakes_external",
    "wakes_coalesced", "pump_sweeps", "deadline_sweeps", "idle_cap_wakes",
    "wake_to_pump_max_us", "wake_to_pump_total_us", "wake_to_pump_samples",
    "service_passes", "flush_sends", "flush_bytes",
    "pump_turns", "pump_messages", "pump_bytes",
    "wake_requests_push", "wake_requests_credit",
    "enq_demand", "enq_undemand", "enq_done", "enq_ack",
    "enq_obj", "enq_obj_open", "enq_obj_chunk", "enq_obj_end",
    "enq_obj_reset", "enq_grp_reset", "enq_grp_evict", "enq_sg_seal",
    "channel_entries_hwm", "channel_bytes_hwm",
)


def _strict_u64(text):
    """ASCII digits only, value <= UINT64_MAX — the exact C grammar.
    int() alone would accept '+1', '1_0', unicode digits, and arbitrary
    magnitude that the C parser rejects (ERANGE)."""
    if not text or any(c < "0" or c > "9" for c in text):
        return None
    v = int(text)
    return v if v <= 0xFFFFFFFFFFFFFFFF else None


def parse_lane_stats_row(line):
    """One lane-stats line (V1, V2, or V3) -> dict (with a "version" key),
    "refused", or None (malformed). Mirrors the C grammar: every key
    exactly once in order, bounded u64 values (lane bounded u32), and the
    ,eor=1 terminator (a row truncated mid-number is malformed). The
    versions are parsed but NEVER pooled — the version joins the
    fingerprint."""
    parts = line.strip().split(",")
    if not parts:
        return None
    if parts[0] == LANE_STATS_PREFIX:
        version, keys = 1, LANE_STATS_KEYS
    elif parts[0] == LANE_STATS_PREFIX_V2:
        version, keys = 2, LANE_STATS_KEYS + TURN_OUTCOME_KEYS
    elif parts[0] == LANE_STATS_PREFIX_V3:
        version, keys = 3, (LANE_STATS_KEYS + TURN_OUTCOME_KEYS +
                            LOCAL_WAKE_KEYS)
    else:
        return None
    kv = parts[1:]
    if len(kv) >= 2 and kv[0].startswith("lane=") and \
            kv[1].startswith("refused="):
        return "refused"
    if len(kv) != 1 + len(keys) + 1:
        return None
    row = {"version": version}
    if not kv[0].startswith("lane="):
        return None
    lane = _strict_u64(kv[0][5:])
    if lane is None or lane > 0xFFFFFFFF:
        return None
    row["lane"] = lane
    for i, key in enumerate(keys):
        cell = kv[1 + i]
        if not cell.startswith(key + "="):
            return None
        v = _strict_u64(cell[len(key) + 1:])
        if v is None:
            return None
        row[key] = v
    if kv[-1] != "eor=1":
        return None
    return row


def load_lane_record(log_path, expected_lanes, text=None):
    """Extract and validate one rep's lane-stat record from its relay log.
    Returns (rows, None) or (None, reason). A refusal, a malformed row, a
    duplicate/missing lane, or a count mismatch rejects the rep — a refused
    snapshot can never read as valid zeros."""
    try:
        content = text if text is not None else open(log_path).read()
    except OSError as e:
        return None, "unreadable relay log %s: %s" % (log_path, e)
    rows = []
    for ln in content.splitlines():
        if not (ln.startswith(LANE_STATS_PREFIX) or
                ln.startswith(LANE_STATS_PREFIX_V2) or
                ln.startswith(LANE_STATS_PREFIX_V3)):
            continue
        parsed = parse_lane_stats_row(ln)
        if parsed == "refused":
            return None, "refused lane snapshot"
        if parsed is None:
            return None, "malformed lane-stat row"
        rows.append(parsed)
    if len(rows) != expected_lanes:
        return None, "lane-stat rows %d != configured lanes %d" % (
            len(rows), expected_lanes)
    lanes_seen = [r["lane"] for r in rows]
    if lanes_seen != list(range(expected_lanes)):
        return None, "duplicate/missing/out-of-order lane rows %r" % \
            lanes_seen
    if len({r["version"] for r in rows}) != 1:
        return None, "mixed lane-row versions"
    return rows, None


def parse_pair_stats_row(line):
    """One RELAY_PAIR_STATS_V1 line -> dict, "refused", or None."""
    parts = line.strip().split(",")
    if not parts or parts[0] != PAIR_STATS_PREFIX:
        return None
    kv = parts[1:]
    if len(kv) >= 3 and kv[0].startswith("src=") and \
            kv[1].startswith("dst=") and kv[2].startswith("refused="):
        return "refused"
    if len(kv) != 2 + len(PAIR_STATS_KEYS) + 1:
        return None
    row = {}
    for idx, name in ((0, "src"), (1, "dst")):
        if not kv[idx].startswith(name + "="):
            return None
        v = _strict_u64(kv[idx][len(name) + 1:])
        if v is None or v > 0xFFFFFFFF:
            return None
        row[name] = v
    for i, key in enumerate(PAIR_STATS_KEYS):
        cell = kv[2 + i]
        if not cell.startswith(key + "="):
            return None
        v = _strict_u64(cell[len(key) + 1:])
        if v is None:
            return None
        row[key] = v
    if kv[-1] != "eor=1":
        return None
    return row


def parse_run_config_row(line):
    """One RELAY_RUN_CONFIG_V1 line -> dict, "refused", or None."""
    parts = line.strip().split(",")
    if not parts or parts[0] != RUN_CONFIG_PREFIX:
        return None
    kv = parts[1:]
    if len(kv) >= 1 and kv[0].startswith("refused="):
        return "refused"
    if len(kv) != len(RUN_CONFIG_KEYS) + 1:
        return None
    row = {}
    for i, key in enumerate(RUN_CONFIG_KEYS):
        cell = kv[i]
        if not cell.startswith(key + "="):
            return None
        v = _strict_u64(cell[len(key) + 1:])
        if v is None or v == 0:
            return None   # the resolver guarantees nonzero
        if key in ("pump_turn_messages", "demand_channel_entries") and \
                v > 0xFFFFFFFF:
            return None   # uint32_t in the runtime
        row[key] = v
    if kv[-1] != "eor=1":
        return None
    return row


def load_relay_record(log_path, expected_lanes, text=None):
    """The STRICT per-rep relay record: lane rows + (for V2-era logs) the
    run-config record and the complete K*(K-1) pair record. Returns
    (record, None) or (None, reason); record = {"lanes": [...], "pairs":
    [...], "config": dict-or-"legacy", "version": 1|2|3}.

    Version rule: if ANY V2+ lane row, pair row, or run-config line is
    present the log is a V2-era log and the FULL strict record is required
    (exactly one valid config record; exactly K*(K-1) pair rows in
    src-major ascending non-self order; single-version V2+ lane rows). A
    log with only V1 lane rows is a legacy record (lanes only, config =
    "legacy"). Versions never pool: the version and the resolved config
    join the fingerprint."""
    try:
        content = text if text is not None else open(log_path).read()
    except OSError as e:
        return None, "unreadable relay log %s: %s" % (log_path, e)
    lanes, pairs, configs = [], [], []
    for ln in content.splitlines():
        if ln.startswith(LANE_STATS_PREFIX) or \
                ln.startswith(LANE_STATS_PREFIX_V2) or \
                ln.startswith(LANE_STATS_PREFIX_V3):
            parsed = parse_lane_stats_row(ln)
            if parsed == "refused":
                return None, "refused lane snapshot"
            if parsed is None:
                return None, "malformed lane-stat row"
            lanes.append(parsed)
        elif ln.startswith(PAIR_STATS_PREFIX):
            parsed = parse_pair_stats_row(ln)
            if parsed == "refused":
                return None, "refused pair snapshot"
            if parsed is None:
                return None, "malformed pair-stat row"
            pairs.append(parsed)
        elif ln.startswith(RUN_CONFIG_PREFIX):
            parsed = parse_run_config_row(ln)
            if parsed == "refused":
                return None, "refused run-config record"
            if parsed is None:
                return None, "malformed run-config record"
            configs.append(parsed)
    if len(lanes) != expected_lanes:
        return None, "lane-stat rows %d != configured lanes %d" % (
            len(lanes), expected_lanes)
    if [r["lane"] for r in lanes] != list(range(expected_lanes)):
        return None, "duplicate/missing/out-of-order lane rows"
    versions = {r["version"] for r in lanes}
    if len(versions) != 1:
        return None, "mixed lane-row versions"
    version = versions.pop()
    v2_era = version >= 2 or pairs or configs
    if not v2_era:
        return {"lanes": lanes, "pairs": [], "config": "legacy",
                "version": 1}, None
    if version < 2:
        return None, "V1 lane rows in a V2-era log"
    if len(configs) != 1:
        return None, "run-config records %d != 1" % len(configs)
    want = [(src, dst)
            for src in range(expected_lanes)
            for dst in range(expected_lanes) if dst != src]
    got = [(r["src"], r["dst"]) for r in pairs]
    if got != want:
        return None, "pair record mismatch (want %d src-major non-self "             "rows, got %r)" % (len(want), got)
    return {"lanes": lanes, "pairs": pairs, "config": configs[0],
            "version": version}, None


def lane_avg_us(row):
    """total/samples; None when samples == 0 (UNAVAILABLE, never zero)."""
    if row["wake_to_pump_samples"] == 0:
        return None
    return row["wake_to_pump_total_us"] / row["wake_to_pump_samples"]


# Per-rep aggregate across lanes: SUM for event counters; MAX for the
# worst-delay and high-water gauges (summing a max across lanes is
# meaningless); weighted ratios recomputed from the summed numerators/
# denominators (a mean of per-lane averages would weight a 1-sample lane
# equally with a 100-sample lane).
_AGG_MAX_KEYS = ("wake_to_pump_max_us", "channel_entries_hwm",
                 "channel_bytes_hwm")
_AGG_SUM_KEYS = tuple(k for k in LANE_STATS_KEYS if k not in _AGG_MAX_KEYS)


def aggregate_lane_rows(lanes):
    out = {}
    for k in _AGG_SUM_KEYS:
        out[k] = sum(r[k] for r in lanes)
    out["max_wake_us"] = max(r["wake_to_pump_max_us"] for r in lanes)
    out["channel_entries_hwm"] = max(r["channel_entries_hwm"]
                                     for r in lanes)
    out["channel_bytes_hwm"] = max(r["channel_bytes_hwm"] for r in lanes)
    out["avg_wake_us"] = (out["wake_to_pump_total_us"] /
                          out["wake_to_pump_samples"]
                          if out["wake_to_pump_samples"] else None)
    arms = (out["wakes_same_lane"] + out["wakes_cross_lane"] +
            out["wakes_external"])
    out["coalesce_ratio"] = out["wakes_coalesced"] / arms if arms else 0.0
    out["msgs_per_sweep"] = (out["pump_messages"] /
                             max(1, out["pump_sweeps"]))
    for k in TURN_OUTCOME_KEYS + LOCAL_WAKE_KEYS:
        out[k] = (sum(r[k] for r in lanes)
                  if all(k in r for r in lanes) else None)
    return out


def summarize_lane_counters(reps, expect_reps=None):
    """Per (impl, workload): per-lane then aggregate med/min/max of the
    attribution metrics across reps. `reps` = list of dicts
    {report, lanes:[rows]}. Under --expect-reps a group holding a different
    rep count is WITHHELD in both sections — an under-replicated subset
    must never read as the campaign's counters."""
    groups = {}
    for rep in reps:
        r = rep["report"]
        groups.setdefault((r["impl"], r["fingerprint"]), []).append(rep)
    ordered = sorted(groups, key=lambda k: (k[0], str(k[1])))

    def replicated(key):
        return expect_reps is None or len(groups[key]) == expect_reps

    def emit(prefix, metric, vals):
        fin = [v for v in vals if v is not None]
        if not fin:
            print("%s,%s,unavailable,unavailable,unavailable" %
                  (prefix, metric))
            return
        # ratios need more precision than event counts (%.1f would render
        # a 0.032 coalescing ratio as 0.0)
        fmt = "%.3f" if metric == "coalesce_ratio" else "%.1f"
        print("%s,%s,%s,%s,%s" %
              (prefix, metric, fmt % median(fin), fmt % min(fin),
               fmt % max(fin)))

    metrics = (
        "wakes_same_lane", "wakes_cross_lane", "wakes_external",
        "wakes_coalesced", "wake_to_pump_samples", "coalesce_ratio",
        "avg_wake_us", "max_wake_us", "pump_sweeps", "pump_turns",
        "pump_messages", "pump_bytes", "msgs_per_sweep",
        "wake_requests_push", "wake_requests_credit", "wake_requests_local",
        "service_passes",
        "flush_sends", "flush_bytes", "idle_cap_wakes", "deadline_sweeps",
        "channel_entries_hwm", "channel_bytes_hwm",
        "enq_demand", "enq_undemand", "enq_done", "enq_ack",
        "enq_obj", "enq_obj_open", "enq_obj_chunk", "enq_obj_end",
        "enq_obj_reset", "enq_grp_reset", "enq_grp_evict", "enq_sg_seal",
    ) + TURN_OUTCOME_KEYS

    print("## lane counters (med/min/max across reps; avg_wake_us "
          "unavailable when zero samples)")
    print("impl,workload,lane,metric,med,min,max")
    for key in ordered:
        rs = groups[key]
        # rows carry the canonical workload label — two workload variants
        # under one impl must never emit indistinguishable counter rows
        wl = fp_label(key[1])
        if not replicated(key):
            print("%s,%s,all,withheld (reps %d != expected %d),"
                  "none,none,none" % (key[0], wl, len(rs), expect_reps))
            continue
        nlanes = len(rs[0]["lanes"])
        for lane in range(nlanes):
            # single-lane "aggregate" = identity sums + the same derived
            # ratios — one code path for both sections
            per = [aggregate_lane_rows([rep["lanes"][lane]]) for rep in rs]
            pfx = "%s,%s,%d" % (key[0], wl, lane)
            for metric in metrics:
                emit(pfx, metric, [a[metric] for a in per])

    print("## aggregate counters (per-rep across lanes: sums for event "
          "counters; MAX for max_wake_us and channel HWMs; "
          "avg_wake_us = sum(total_us)/sum(samples); coalesce_ratio = "
          "sum(coalesced)/sum(all wake arms); med/min/max across reps)")
    print("impl,workload,metric,med,min,max")
    for key in ordered:
        rs = groups[key]
        wl = fp_label(key[1])
        if not replicated(key):
            print("%s,%s,withheld (reps %d != expected %d),none,none,none"
                  % (key[0], wl, len(rs), expect_reps))
            continue
        aggs = [aggregate_lane_rows(rep["lanes"]) for rep in rs]
        for metric in metrics:
            emit("%s,%s" % (key[0], wl), metric,
                 [a[metric] for a in aggs])


def load_report(path, text=None):
    """Parse and strictly validate one blast report."""
    try:
        d = json.loads(text if text is not None else open(path).read())
    except (OSError, ValueError) as e:
        die("unreadable/malformed report %s: %s" % (path, e))
    try:
        metrics = d["metrics"]
        fairness = d["fairness"]
        correctness = d["correctness"]
    except (KeyError, TypeError) as e:
        die("report %s missing section: %s" % (path, e))
    impl = _req_str(d, "impl", path)
    basis = metrics.get("measurement_basis")
    if basis != REQUIRED_BASIS:
        die("report %s has measurement_basis '%s' (want '%s') — "
            "classes are never merged" % (path, basis, REQUIRED_BASIS))
    retrieval = _retrieval_mode(d, path)
    alpn = _req_str(fairness, "alpn", path)
    counts = {f: _req_count(fairness, f, path) for f in FP_COUNT_FIELDS}
    fingerprint = (("retrieval", retrieval), ("alpn", alpn)) + \
        tuple(sorted(counts.items()))
    subs = counts["subscribers_per_track"] * counts["namespace_count"] * \
        counts["tracks_per_namespace"]
    expected = subs * counts["groups"] * counts["objects_per_group"]
    if expected == 0:
        die("report %s: workload expects zero deliveries "
            "(a diagnostic fraction over zero is meaningless)" % path)
    return {
        "impl": impl,
        "fingerprint": fingerprint,
        "passed": _req_bool(correctness, "passed", path),
        "objects_delivered": _req_count(metrics, "objects_delivered", path),
        "bytes_delivered": _req_count(metrics, "bytes_delivered", path),
        "expected_deliveries": expected,
        "wall_objects_per_sec": _req_num(metrics, "wall_objects_per_sec",
                                         path),
        "latency_p50_ns": _req_num(metrics, "latency_p50_ns", path),
        "latency_p99_ns": _req_num(metrics, "latency_p99_ns", path),
    }


def fp_label(fingerprint):
    d = dict(fingerprint)
    label = "%s %s subs=%d objs=%d" % (
        d["retrieval"], d["alpn"],
        d["subscribers_per_track"] * d["namespace_count"] *
        d["tracks_per_namespace"],
        d["groups"] * d["objects_per_group"])
    # paired-mode runs carry the lane-stats version and the RESOLVED
    # operating point in the fingerprint: distinct budgets/capacities (or
    # a legacy V1 log) must stay visibly distinct in every emitted row
    if "lane_stats_version" in d:
        label += " v%d" % d["lane_stats_version"]
        if d.get("pump_turn_messages") is not None:
            label += " cfg=%d/%d/%d/%d" % (
                d["pump_turn_messages"], d["pump_turn_bytes"],
                d["demand_channel_entries"], d["demand_channel_bytes"])
        else:
            label += " cfg=legacy"
    return label


def summarize(reports, expect_reps=None):
    groups = {}
    for r in reports:
        groups.setdefault((r["impl"], r["fingerprint"]), []).append(r)
    print("impl,workload,attempted,completed,passed,"
          "canon_wall_obj_s_med,canon_wall_obj_s_min,canon_wall_obj_s_max,"
          "canon_p50_ms_med,canon_p99_ms_med,"
          "diag_delivery_frac_med,diag_partial_obj_s_med,"
          "diag_recv_only_p99_ms_med")
    for key in sorted(groups, key=lambda k: (k[0], str(k[1]))):
        rs = groups[key]
        ok = [r for r in rs if r["passed"]]
        bad = [r for r in rs if not r["passed"]]
        attempted = expect_reps if expect_reps is not None else len(rs)
        # Replication policy under --expect-reps: canonical output requires the
        # group to hold EXACTLY the attempted count AND every one of them to
        # have passed. Fewer completed = outer timeouts/launcher failures;
        # fewer passed = correctness failures — either way a surviving subset
        # must not masquerade as the campaign's median (survivorship bias).
        # More than attempted = duplicated/over-replicated input, refused
        # rather than silently weighted.
        if expect_reps is not None:
            canonical_ok = (len(rs) == expect_reps and
                            len(ok) == expect_reps)
        else:
            canonical_ok = bool(ok)
        # Canonical performance: correctness-passed rows ONLY, full passing
        # replication under --expect-reps. Otherwise none, never a number.
        if canonical_ok:
            rates = [r["wall_objects_per_sec"] for r in ok]
            canon = "%.1f,%.1f,%.1f,%.2f,%.2f" % (
                median(rates), min(rates), max(rates),
                median([r["latency_p50_ns"] / 1e6 for r in ok]),
                median([r["latency_p99_ns"] / 1e6 for r in ok]))
        else:
            canon = "none,none,none,none,none"
        # Diagnostics: failed rows ONLY, explicitly labeled. Their rate is a
        # CENSORED partial-progress figure (through the last received object)
        # and their latency covers received objects only.
        if bad:
            frac = [r["objects_delivered"] / r["expected_deliveries"]
                    for r in bad]
            diag = "%.3f,%.1f,%.2f" % (
                median(frac),
                median([r["wall_objects_per_sec"] for r in bad]),
                median([r["latency_p99_ns"] / 1e6 for r in bad]))
        else:
            diag = "none,none,none"
        print("%s,%s,%s,%d,%d,%s,%s" %
              (key[0], fp_label(key[1]), attempted, len(rs), len(ok), canon,
               diag))


def _mk(impl, rate, p50, p99, basis=REQUIRED_BASIS, passed=True,
        delivered=16384, subs=16, groups=16, opg=64, alpn="moqt-16",
        raw_passed=None, retrieval_notes=None):
    notes = retrieval_notes if retrieval_notes is not None else \
        ["retrieval=subscribe (live ABSOLUTE_START fan-out)"]
    return json.dumps({
        "impl": impl,
        "correctness": {"passed": passed if raw_passed is None else raw_passed},
        "ergonomics_notes": notes,
        "fairness": {
            "alpn": alpn, "namespace_count": 1, "tracks_per_namespace": 1,
            "publishers_per_namespace": 1, "subscribers_per_track": subs,
            "groups": groups, "objects_per_group": opg,
            "first_object_size": 512, "other_object_size": 512,
        },
        "metrics": {
            "measurement_basis": basis,
            "objects_delivered": delivered,
            "bytes_delivered": delivered * 512,
            "wall_objects_per_sec": rate,
            "latency_p50_ns": p50,
            "latency_p99_ns": p99,
        },
    })


def _capture_summary(reports, expect_reps=None):
    import io
    buf = io.StringIO()
    old = sys.stdout
    sys.stdout = buf
    try:
        summarize(reports, expect_reps)
    finally:
        sys.stdout = old
    return buf.getvalue()


def selftest():
    fails = 0

    # (a) canonical medians use passed rows only: a failed outlier with a wild
    # rate cannot move them.
    texts = [_mk("moq5-lanes1", r, 1e6, 5e6) for r in (90000.0, 100000.0,
                                                       110000.0)]
    texts.append(_mk("moq5-lanes1", 5.0, 1e6, 9e9, passed=False,
                     delivered=100))
    rs = [load_report("<m%d>" % i, t) for i, t in enumerate(texts)]
    out = _capture_summary(rs)
    line = [ln for ln in out.splitlines() if ln.startswith("moq5-lanes1")][0]
    cells = line.split(",")
    if cells[5] == "100000.0" and cells[4] == "3":
        print("PASS: failed outlier cannot affect passed medians")
    else:
        sys.stderr.write("FAIL: outlier moved canonical median: %s\n" % line)
        fails += 1

    # (b) zero passing reps -> canonical none; diagnostics labeled separately.
    texts = [_mk("moq5-lanes2", 2306.0, 6e7, 5.2e9, passed=False,
                 delivered=12456) for _ in range(5)]
    rs = [load_report("<m%d>" % i, t) for i, t in enumerate(texts)]
    out = _capture_summary(rs)
    line = [ln for ln in out.splitlines() if ln.startswith("moq5-lanes2")][0]
    cells = line.split(",")
    if cells[5] == "none" and cells[9] == "none" and cells[10] == "0.760" and \
       cells[11] == "2306.0":
        print("PASS: zero-pass group has no canonical result "
              "(diagnostics labeled)")
    else:
        sys.stderr.write("FAIL: zero-pass line %s\n" % line)
        fails += 1

    # (c) mixed workloads never pool: different ALPN, subscriber count, or
    # RETRIEVAL MODE form DISTINCT groups under the same impl label.
    rs = [load_report("<a>", _mk("relay", 1000.0, 1e6, 2e6)),
          load_report("<b>", _mk("relay", 2000.0, 1e6, 2e6, alpn="moqt-18")),
          load_report("<c>", _mk("relay", 3000.0, 1e6, 2e6, subs=8)),
          load_report("<d>", _mk("relay", 4000.0, 1e6, 2e6,
                                 retrieval_notes=["retrieval=fetch (cache)"]))]
    out = _capture_summary(rs)
    n = len([ln for ln in out.splitlines() if ln.startswith("relay")])
    if n == 4:
        print("PASS: mixed ALPN/scenario/retrieval reports form separate "
              "groups")
    else:
        sys.stderr.write("FAIL: mixed rows pooled into %d group(s):\n%s" %
                         (n, out))
        fails += 1

    # (d) survivorship bias: 5 attempted reps but only 1 completed report ->
    # canonical none (attempted/completed both visible).
    rs = [load_report("<s>", _mk("moq5-lanes1", 100000.0, 1e6, 5e6))]
    out = _capture_summary(rs, expect_reps=5)
    line = [ln for ln in out.splitlines() if ln.startswith("moq5-lanes1")][0]
    cells = line.split(",")
    if cells[2] == "5" and cells[3] == "1" and cells[5] == "none":
        print("PASS: under-replicated group (1 of 5 attempted) emits no "
              "canonical result")
    else:
        sys.stderr.write("FAIL: survivor-only line %s\n" % line)
        fails += 1

    # (d2) survivorship bias over correctness failures: 5 completed reports
    # with only 1 pass -> canonical none (the single pass must not become the
    # campaign's "median"). Diagnostics still summarize the 4 failures.
    texts = [_mk("moq5-mixed", 100000.0, 1e6, 5e6)]
    texts += [_mk("moq5-mixed", 2000.0, 1e6, 5e9, passed=False,
                  delivered=12000) for _ in range(4)]
    rs = [load_report("<m%d>" % i, t) for i, t in enumerate(texts)]
    out = _capture_summary(rs, expect_reps=5)
    line = [ln for ln in out.splitlines() if ln.startswith("moq5-mixed")][0]
    cells = line.split(",")
    if cells[3] == "5" and cells[4] == "1" and cells[5] == "none" and \
       cells[10] != "none":
        print("PASS: mixed-success group (1 pass of 5 completed) emits no "
              "canonical result")
    else:
        sys.stderr.write("FAIL: mixed-success line %s\n" % line)
        fails += 1

    # (d3) over-replication: 6 reports supplied under --expect-reps 5 ->
    # canonical none (duplicated input is refused, never silently weighted).
    texts = [_mk("moq5-over", 100000.0, 1e6, 5e6) for _ in range(6)]
    rs = [load_report("<o%d>" % i, t) for i, t in enumerate(texts)]
    out = _capture_summary(rs, expect_reps=5)
    line = [ln for ln in out.splitlines() if ln.startswith("moq5-over")][0]
    cells = line.split(",")
    if cells[2] == "5" and cells[3] == "6" and cells[5] == "none":
        print("PASS: over-replicated group (6 supplied, 5 expected) emits no "
              "canonical result")
    else:
        sys.stderr.write("FAIL: over-replication line %s\n" % line)
        fails += 1

    # (e) a report from another measurement class is a hard error.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1, basis="in_process_wall"))
        sys.stderr.write("FAIL: wrong-basis report accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: wrong measurement_basis rejected (classes never merged)")

    # (f) malformed JSON is a hard error.
    try:
        load_report("<mem>", "{not json")
        sys.stderr.write("FAIL: malformed JSON accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: malformed report rejected")

    # (g) missing retrieval metadata is ambiguous -> hard error; so are
    # conflicting notes.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1, retrieval_notes=[]))
        sys.stderr.write("FAIL: missing retrieval metadata accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: missing retrieval metadata rejected")
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1, retrieval_notes=[
            "retrieval=subscribe (x)", "retrieval=fetch (y)"]))
        sys.stderr.write("FAIL: conflicting retrieval metadata accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: conflicting retrieval metadata rejected")
    # Two IDENTICAL retrieval notes are still not "exactly one": note COUNT is
    # enforced, not distinct-mode count.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1, retrieval_notes=[
            "retrieval=subscribe (x)", "retrieval=subscribe (x)"]))
        sys.stderr.write("FAIL: duplicate identical retrieval notes accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: duplicate identical retrieval notes rejected")

    # (h) correctness.passed must be a JSON boolean: the STRING "false" (truthy
    # in python) is rejected, never treated as passed.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1, raw_passed="false"))
        sys.stderr.write("FAIL: string 'false' accepted as passed\n")
        fails += 1
    except SystemExit:
        print("PASS: non-boolean correctness.passed rejected")

    # (i) non-finite metrics are rejected.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1).replace(
            '"wall_objects_per_sec": 1.0', '"wall_objects_per_sec": NaN'))
        sys.stderr.write("FAIL: NaN metric accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: non-finite metric rejected")

    # (j) negative delivery counts are rejected.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1, delivered=-5))
        sys.stderr.write("FAIL: negative count accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: negative delivery count rejected")

    # (k) fingerprint fields are strictly typed: a list-valued alpn is a
    # schema error, not an unhashable-key traceback.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1).replace(
            '"alpn": "moqt-16"', '"alpn": ["moqt-16"]'))
        sys.stderr.write("FAIL: list-valued alpn accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: non-string fingerprint field rejected")

    # (l) a zero-expected-delivery workload is rejected outright.
    try:
        load_report("<mem>", _mk("x", 1.0, 1, 1, groups=0))
        sys.stderr.write("FAIL: zero-expected workload accepted\n")
        fails += 1
    except SystemExit:
        print("PASS: zero expected-delivery workload rejected")

    # --- lane-stat record integration -------------------------------------
    def _lane_row(lane, cross=0, samples=0, total=0):
        vals = {k: 0 for k in LANE_STATS_KEYS}
        vals["wakes_cross_lane"] = cross
        vals["wake_to_pump_samples"] = samples
        vals["wake_to_pump_total_us"] = total
        body = ",".join("%s=%d" % (k, vals[k]) for k in LANE_STATS_KEYS)
        return "%s,lane=%d,%s,eor=1" % (LANE_STATS_PREFIX, lane, body)

    # (m1) a valid two-lane record parses; averages honor total/samples.
    log = "noise\n%s\n%s\nnoise" % (_lane_row(0, cross=5, samples=5,
                                              total=1000),
                                    _lane_row(1))
    rows, why = load_lane_record("<log>", 2, text=log)
    if rows is not None and rows[0]["wakes_cross_lane"] == 5 and \
       lane_avg_us(rows[0]) == 200.0 and lane_avg_us(rows[1]) is None:
        print("PASS: valid two-lane record (avg=total/samples; zero "
              "samples unavailable)")
    else:
        sys.stderr.write("FAIL: two-lane record %r %r\n" % (rows, why))
        fails += 1

    # (m2) missing lane row -> rejected.
    rows, why = load_lane_record("<log>", 2, text=_lane_row(0))
    if rows is None and "!= configured lanes" in why:
        print("PASS: missing lane row rejects the rep")
    else:
        sys.stderr.write("FAIL: missing-lane %r %r\n" % (rows, why))
        fails += 1

    # (m3) duplicate lane row -> rejected.
    log = "%s\n%s" % (_lane_row(0), _lane_row(0))
    rows, why = load_lane_record("<log>", 2, text=log)
    if rows is None and "duplicate" in why:
        print("PASS: duplicate lane row rejects the rep")
    else:
        sys.stderr.write("FAIL: duplicate-lane %r %r\n" % (rows, why))
        fails += 1

    # (m4) a refused snapshot rejects the rep — it can never read as zeros.
    log = "%s\n%s,lane=1,refused=adapter" % (_lane_row(0),
                                             LANE_STATS_PREFIX)
    rows, why = load_lane_record("<log>", 2, text=log)
    if rows is None and "refused" in why:
        print("PASS: refused snapshot rejects the rep (never zeros)")
    else:
        sys.stderr.write("FAIL: refused-snapshot %r %r\n" % (rows, why))
        fails += 1

    # (m5) a failed blast rep with an otherwise valid lane record: the record
    # parses (counters remain usable for diagnostics) while the report's
    # correctness failure keeps it out of canonical aggregates.
    log = "%s\n%s" % (_lane_row(0, cross=3, samples=3, total=600000),
                      _lane_row(1, cross=2, samples=2, total=400000))
    rows, why = load_lane_record("<log>", 2, text=log)
    failed_rep = load_report("<m>", _mk("moq5-lanes2", 2306.0, 6e7, 5.2e9,
                                        passed=False, delivered=12456))
    if rows is not None and not failed_rep["passed"] and \
       lane_avg_us(rows[0]) == 200000.0:
        print("PASS: failed blast rep keeps a usable lane record "
              "(diagnostics only)")
    else:
        sys.stderr.write("FAIL: failed-blast record %r %r\n" % (rows, why))
        fails += 1

    # (m6) a truncated row (mid-number, no eor terminator) is malformed.
    trunc = _lane_row(0)[:-10]
    if parse_lane_stats_row(trunc) is None:
        print("PASS: truncated lane row malformed (eor terminator)")
    else:
        sys.stderr.write("FAIL: truncated row accepted\n")
        fails += 1

    # (m7) numeric bounds mirror the C parser: exact UINT64_MAX parses,
    # UINT64_MAX+1 is malformed (never saturated); lane bounded at
    # UINT32_MAX; only plain ASCII digits (no '1_0', '+1', spaces).
    u64max = "18446744073709551615"
    maxrow = _lane_row(0).replace(",wakes_same_lane=0,",
                                  ",wakes_same_lane=%s," % u64max, 1)
    overrow = _lane_row(0).replace(",wakes_same_lane=0,",
                                   ",wakes_same_lane=18446744073709551616,", 1)
    junkrow = _lane_row(0).replace(",wakes_same_lane=0,",
                                   ",wakes_same_lane=1_0,", 1)
    lane_max = parse_lane_stats_row(_lane_row(4294967295))
    r7 = parse_lane_stats_row(maxrow)
    if (isinstance(r7, dict) and r7["wakes_same_lane"] == 2**64 - 1 and
            parse_lane_stats_row(overrow) is None and
            parse_lane_stats_row(junkrow) is None and
            isinstance(lane_max, dict) and lane_max["lane"] == 2**32 - 1 and
            parse_lane_stats_row(_lane_row(4294967296)) is None):
        print("PASS: u64/u32 bounds enforced (max ok, max+1 malformed)")
    else:
        sys.stderr.write("FAIL: numeric bounds %r\n" % (r7,))
        fails += 1

    # (m8) paired mode is all-or-nothing: ANY rejected lane record fails the
    # whole invocation (exit 2) and emits NO lane-counter aggregate — a
    # surviving subset must never masquerade as the campaign's counters.
    import io
    import os
    import tempfile

    def _run_main(bad_reps):
        with tempfile.TemporaryDirectory() as td:
            args = ["relay_blast_summarize.py", "--expect-reps", "5"]
            for i in range(5):
                jp = os.path.join(td, "r%d.json" % i)
                lp = os.path.join(td, "r%d.log" % i)
                with open(jp, "w") as f:
                    f.write(_mk("moq5-lanes2", 100.0, 1e6, 2e6))
                good = "%s\n%s" % (_lane_row(0), _lane_row(1))
                with open(lp, "w") as f:
                    f.write(_lane_row(0) if i in bad_reps else good)
                args += ["--rep", jp, lp, "2"]
            out_buf, err_buf = io.StringIO(), io.StringIO()
            old_out, old_err = sys.stdout, sys.stderr
            sys.stdout, sys.stderr = out_buf, err_buf
            code = None
            try:
                code = main(args)
            except SystemExit as e:
                code = e.code
            finally:
                sys.stdout, sys.stderr = old_out, old_err
            return code, out_buf.getvalue()

    code1, out1 = _run_main({2})
    code2, out2 = _run_main({0, 1, 2, 3, 4})
    if (code1 == 2 and "## lane counters" not in out1 and
            code2 == 2 and "## lane counters" not in out2):
        print("PASS: any rejected paired record fails the invocation "
              "(1-of-5 and all-invalid)")
    else:
        sys.stderr.write("FAIL: paired-mode rejection codes %r/%r "
                         "lane-output %r/%r\n" %
                         (code1, code2, "## lane counters" in out1,
                          "## lane counters" in out2))
        fails += 1

    # (m9) aggregate counters: per-rep aggregation is sum for counters,
    # MAX (not sum) for max_wake_us and channel HWMs, weighted (not
    # mean-of-means) for avg_wake_us, and coalesced-over-all-arms for the
    # coalescing ratio.
    a0 = {k: 0 for k in LANE_STATS_KEYS}
    a0.update(lane=0, wakes_same_lane=3, wakes_cross_lane=5,
              wakes_external=2, wakes_coalesced=2,
              wake_to_pump_samples=1, wake_to_pump_total_us=100,
              wake_to_pump_max_us=5000,
              channel_entries_hwm=10, channel_bytes_hwm=100)
    a1 = {k: 0 for k in LANE_STATS_KEYS}
    a1.update(lane=1, wake_to_pump_samples=3, wake_to_pump_total_us=900,
              wake_to_pump_max_us=7000,
              channel_entries_hwm=64, channel_bytes_hwm=11264)
    agg_rep = {"report": load_report("<m>", _mk("agg-impl", 100.0, 1e6,
                                                2e6)),
               "lanes": [a0, a1]}
    buf = io.StringIO()
    old_out = sys.stdout
    sys.stdout = buf
    try:
        summarize_lane_counters([agg_rep, agg_rep], expect_reps=2)
    finally:
        sys.stdout = old_out
    out = buf.getvalue()
    awl = fp_label(agg_rep["report"]["fingerprint"])
    want = (  # 1000/4 not 200; max not 12000; 2/(3+5+2); max not 74
        "agg-impl,%s,avg_wake_us,250.0,250.0,250.0" % awl,
        "agg-impl,%s,max_wake_us,7000.0,7000.0,7000.0" % awl,
        "agg-impl,%s,coalesce_ratio,0.200,0.200,0.200" % awl,
        "agg-impl,%s,channel_entries_hwm,64.0,64.0,64.0" % awl)
    missing = [w for w in want if w not in out]
    if "## aggregate counters" in out and not missing:
        print("PASS: aggregate counters (weighted avg, max-not-sum, "
              "arm-denominator coalescing)")
    else:
        sys.stderr.write("FAIL: aggregate section missing %r\n" % missing)
        fails += 1

    # (m10) lane counters are withheld for a group that is not fully
    # replicated (never a smaller-sample aggregate without a label).
    buf = io.StringIO()
    sys.stdout = buf
    try:
        summarize_lane_counters([agg_rep], expect_reps=2)
    finally:
        sys.stdout = old_out
    out = buf.getvalue()
    if "withheld" in out and "avg_wake_us,250.0" not in out:
        print("PASS: under-replicated group withholds lane counters")
    else:
        sys.stderr.write("FAIL: under-replicated group emitted counters\n")
        fails += 1

    # (m11) counter rows carry the workload fingerprint: two workload
    # variants under ONE impl (here 16-sub vs 8-sub topologies) must emit
    # distinguishable per-lane and aggregate rows, never pooled or
    # ambiguous.
    rep_a = {"report": load_report("<m>", _mk("agg-impl", 100.0, 1e6, 2e6)),
             "lanes": [a0, a1]}
    rep_b = {"report": load_report("<m>", _mk("agg-impl", 100.0, 1e6, 2e6,
                                              subs=8, delivered=8192)),
             "lanes": [a1, a1]}
    buf = io.StringIO()
    sys.stdout = buf
    try:
        summarize_lane_counters([rep_a, rep_b])
    finally:
        sys.stdout = old_out
    out = buf.getvalue()
    la = fp_label(rep_a["report"]["fingerprint"])
    lb = fp_label(rep_b["report"]["fingerprint"])
    want_a = "agg-impl,%s,avg_wake_us,250.0,250.0,250.0" % la
    want_b = "agg-impl,%s,avg_wake_us,300.0,300.0,300.0" % lb
    if la != lb and want_a in out and want_b in out:
        print("PASS: mixed workload fingerprints stay distinguishable "
              "in counter rows")
    else:
        sys.stderr.write("FAIL: fingerprint dropped from counter rows "
                         "(%r/%r)\n" % (want_a in out, want_b in out))
        fails += 1


    # --- strict V2-era records ---------------------------------------------
    def _lane_row_v2(lane, **kw):
        vals = {k: 0 for k in LANE_STATS_KEYS + TURN_OUTCOME_KEYS}
        vals.update(kw)
        body = ",".join("%s=%d" % (k, vals[k])
                        for k in LANE_STATS_KEYS + TURN_OUTCOME_KEYS)
        return "%s,lane=%d,%s,eor=1" % (LANE_STATS_PREFIX_V2, lane, body)

    def _lane_row_v3(lane, **kw):
        keys = LANE_STATS_KEYS + TURN_OUTCOME_KEYS + LOCAL_WAKE_KEYS
        vals = {k: 0 for k in keys}
        vals.update(kw)
        body = ",".join("%s=%d" % (k, vals[k]) for k in keys)
        return "%s,lane=%d,%s,eor=1" % (LANE_STATS_PREFIX_V3, lane, body)

    def _pair_row(src, dst, **kw):
        vals = {k: 0 for k in PAIR_STATS_KEYS}
        vals.update(kw)
        body = ",".join("%s=%d" % (k, vals[k]) for k in PAIR_STATS_KEYS)
        return "%s,src=%d,dst=%d,%s,eor=1" % (PAIR_STATS_PREFIX, src, dst,
                                              body)

    def _cfg_row(m=64, b=2097152, e=64, cb=8388608):
        return "%s,pump_turn_messages=%d,pump_turn_bytes=%d," \
               "demand_channel_entries=%d,demand_channel_bytes=%d,eor=1" % (
                   RUN_CONFIG_PREFIX, m, b, e, cb)

    # (m12) a complete strict V2 record parses: lanes + config + K*(K-1)
    # pairs; the config values are exposed for the fingerprint join.
    good_v2 = "\n".join([_cfg_row(), _lane_row_v2(0, turns_msg_budget=3),
                         _lane_row_v2(1), _pair_row(0, 1, data_messages=7),
                         _pair_row(1, 0)])
    rec, why = load_relay_record("<log>", 2, text=good_v2)
    if rec is not None and rec["version"] == 2 and \
       rec["config"]["pump_turn_messages"] == 64 and \
       rec["lanes"][0]["turns_msg_budget"] == 3 and \
       rec["pairs"][0]["data_messages"] == 7:
        print("PASS: strict V2 record (config + lanes + full pair set)")
    else:
        sys.stderr.write("FAIL: strict V2 record %r %r\n" % (rec, why))
        fails += 1

    # (m13) config-record cardinality fails closed: zero and duplicate.
    no_cfg = "\n".join([_lane_row_v2(0), _lane_row_v2(1),
                        _pair_row(0, 1), _pair_row(1, 0)])
    rec1, why1 = load_relay_record("<log>", 2, text=no_cfg)
    dup_cfg = _cfg_row() + "\n" + good_v2
    rec2, why2 = load_relay_record("<log>", 2, text=dup_cfg)
    if rec1 is None and "run-config records 0" in why1 and \
       rec2 is None and "run-config records 2" in why2:
        print("PASS: run-config cardinality fails closed (0 and 2)")
    else:
        sys.stderr.write("FAIL: config cardinality %r %r\n" % (why1, why2))
        fails += 1

    # (m13b) resolved-value bounds: zero anywhere and u32 overflow on the
    # message/entry fields are impossible resolver outputs -> malformed;
    # exact maxima parse.
    ok_max = parse_run_config_row(_cfg_row(m=4294967295,
                                           b=2**64 - 1,
                                           e=4294967295,
                                           cb=2**64 - 1))
    bad_rows = [
        _cfg_row(m=0), _cfg_row(b=0), _cfg_row(e=0), _cfg_row(cb=0),
        _cfg_row(m=4294967296), _cfg_row(e=4294967296),
    ]
    if isinstance(ok_max, dict) and \
       ok_max["pump_turn_messages"] == 4294967295 and \
       all(parse_run_config_row(r) is None for r in bad_rows):
        print("PASS: run-config resolved-value bounds "
              "(zero/u32-overflow malformed, maxima parse)")
    else:
        sys.stderr.write("FAIL: run-config bounds\n")
        fails += 1

    # (m14) pair completeness fails closed: missing, duplicate, self,
    # out-of-range, refused.
    base = [_cfg_row(), _lane_row_v2(0), _lane_row_v2(1)]
    cases = {
        "missing": [_pair_row(0, 1)],
        "duplicate": [_pair_row(0, 1), _pair_row(0, 1)],
        "self": [_pair_row(0, 0), _pair_row(1, 0)],
        "range": [_pair_row(0, 2), _pair_row(1, 0)],
        "refused": [_pair_row(0, 1),
                    "%s,src=1,dst=0,refused=shard" % PAIR_STATS_PREFIX],
    }
    bad = [name for name, rows in cases.items()
           if load_relay_record("<log>", 2,
                                text="\n".join(base + rows))[0] is not None]
    if not bad:
        print("PASS: pair completeness fails closed (missing/dup/self/"
              "range/refused)")
    else:
        sys.stderr.write("FAIL: pair completeness admitted %r\n" % bad)
        fails += 1

    # (m15) V1 lane rows in a V2-era log (pair/config present) fail closed;
    # V1-only logs stay a valid LEGACY record.
    mixed = "\n".join([_cfg_row(), _lane_row(0), _lane_row(1),
                       _pair_row(0, 1), _pair_row(1, 0)])
    recm, whym = load_relay_record("<log>", 2, text=mixed)
    legacy = "\n".join([_lane_row(0), _lane_row(1)])
    recl, whyl = load_relay_record("<log>", 2, text=legacy)
    if recm is None and "V2-era" in whym and recl is not None and \
       recl["version"] == 1 and recl["config"] == "legacy":
        print("PASS: V1-in-V2-era fails closed; V1-only stays legacy")
    else:
        sys.stderr.write("FAIL: era rule %r %r %r\n" % (whym, recl, whyl))
        fails += 1

    # (m16) K=1: one V2 lane row + config + ZERO pair rows is complete.
    k1 = "\n".join([_cfg_row(), _lane_row_v2(0)])
    reck, whyk = load_relay_record("<log>", 1, text=k1)
    if reck is not None and reck["pairs"] == [] and reck["version"] == 2:
        print("PASS: K=1 strict record (zero pair rows) is complete")
    else:
        sys.stderr.write("FAIL: K=1 record %r %r\n" % (reck, whyk))
        fails += 1

    # (m18) load_lane_record recognizes EVERY supported row version: a
    # V2-only log (what the multi-lane relay emits) must extract, as must
    # V3; a log mixing row versions is rejected — versions never pool.
    v2_log = "\n".join([_lane_row_v2(0), _lane_row_v2(1)])
    rows2, why2v = load_lane_record("<log>", 2, text=v2_log)
    v3_log = "\n".join([_lane_row_v3(0, wake_requests_local=4),
                        _lane_row_v3(1)])
    rows3, why3v = load_lane_record("<log>", 2, text=v3_log)
    mixed_ver = "\n".join([_lane_row(0), _lane_row_v2(1)])
    rowsm, whymv = load_lane_record("<log>", 2, text=mixed_ver)
    if rows2 is not None and rows2[0]["version"] == 2 and \
       rows3 is not None and rows3[0]["wake_requests_local"] == 4 and \
       rowsm is None and "mixed" in whymv:
        print("PASS: load_lane_record admits V1/V2/V3, rejects mixed")
    else:
        sys.stderr.write("FAIL: version extraction %r %r %r %r\n" %
                         (why2v, why3v, rowsm, whymv))
        fails += 1

    # (m19) the V3 grammar is strict: wake_requests_local present exactly
    # in V3 (a V3-prefixed row without it, or a V2-prefixed row with it,
    # is malformed), and a strict V3 record loads with pairs + config.
    v3_missing = _lane_row_v3(0).replace(",wake_requests_local=0", "")
    v2_extra = _lane_row_v2(0).replace(
        ",eor=1", ",wake_requests_local=0,eor=1")
    good_v3 = "\n".join([_cfg_row(), _lane_row_v3(0, wake_requests_local=2),
                         _lane_row_v3(1), _pair_row(0, 1), _pair_row(1, 0)])
    rec3, why3r = load_relay_record("<log>", 2, text=good_v3)
    if parse_lane_stats_row(v3_missing) is None and \
       parse_lane_stats_row(v2_extra) is None and \
       rec3 is not None and rec3["version"] == 3 and \
       rec3["lanes"][0]["wake_requests_local"] == 2:
        print("PASS: strict V3 grammar + strict V3 record")
    else:
        sys.stderr.write("FAIL: V3 strictness %r %r\n" % (rec3, why3r))
        fails += 1

    # (m17) V1 and V2 never pool: same impl, different eras -> distinct
    # fingerprints and distinct labels.
    fp = load_report("<m>", _mk("agg-impl", 100.0, 1e6, 2e6))["fingerprint"]
    fp_v1 = tuple(fp) + (("lane_stats_version", 1),) + \
        tuple((k, None) for k in RUN_CONFIG_KEYS)
    fp_v2 = tuple(fp) + (("lane_stats_version", 2),) + \
        tuple((k, 64) for k in RUN_CONFIG_KEYS)
    if fp_v1 != fp_v2 and "cfg=legacy" in fp_label(fp_v1) and \
       "v2 cfg=64/64/64/64" in fp_label(fp_v2):
        print("PASS: V1/V2 eras never pool (fingerprint + label distinct)")
    else:
        sys.stderr.write("FAIL: era pooling %r vs %r\n" %
                         (fp_label(fp_v1), fp_label(fp_v2)))
        fails += 1

    if fails == 0:
        print("# ALL PASS")
    return 1 if fails else 0


def main(argv):
    paths = []
    triples = []   # (report.json, relay.log, lanes) — the paired mode
    do_selftest = False
    expect_reps = None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--selftest":
            do_selftest = True
        elif a == "--expect-reps":
            i += 1
            if i >= len(argv):
                die("--expect-reps needs a value")
            try:
                expect_reps = int(argv[i])
            except ValueError:
                die("--expect-reps not an integer: %s" % argv[i])
            if expect_reps < 1:
                die("--expect-reps must be >= 1")
        elif a == "--rep":
            if i + 3 >= len(argv):
                die("--rep needs <report.json> <relay.log> <lanes>")
            try:
                lanes = int(argv[i + 3])
            except ValueError:
                die("--rep lanes not an integer: %s" % argv[i + 3])
            if lanes < 1:
                die("--rep lanes must be >= 1")
            triples.append((argv[i + 1], argv[i + 2], lanes))
            i += 3
        elif a.startswith("--"):
            die("unknown flag: %s" % a)
        else:
            paths.append(a)
        i += 1
    if do_selftest:
        return selftest()
    if triples and paths:
        die("mix of --rep triples and bare report paths")
    if triples:
        reps = []
        rejected = 0
        for (jpath, lpath, lanes) in triples:
            report = load_report(jpath)
            rec, why = load_relay_record(lpath, lanes)
            if rec is None:
                sys.stderr.write("relay_blast_summarize: REJECTED %s: %s\n"
                                 % (jpath, why))
                rejected += 1
                continue
            # the lane-stats version and the RESOLVED operating point join
            # the fingerprint: no two operating points (or eras) can pool
            extra = [("lane_stats_version", rec["version"])]
            if rec["config"] == "legacy":
                extra += [(k, None) for k in RUN_CONFIG_KEYS]
            else:
                extra += [(k, rec["config"][k]) for k in RUN_CONFIG_KEYS]
            report["fingerprint"] = tuple(report["fingerprint"]) + \
                tuple(extra)
            reps.append({"report": report, "lanes": rec["lanes"],
                         "pairs": rec["pairs"], "config": rec["config"]})
        if rejected:
            # All-or-nothing: one invalid lane record invalidates the whole
            # paired campaign — no survivor-subset counters, nonzero exit.
            die("%d of %d paired rep(s) rejected (invalid lane records) — "
                "campaign invalidated, no aggregates emitted"
                % (rejected, len(triples)))
        summarize([r["report"] for r in reps], expect_reps)
        summarize_lane_counters(reps, expect_reps)
        return 0
    if not paths:
        die("no report files given")
    summarize([load_report(p) for p in paths], expect_reps)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
