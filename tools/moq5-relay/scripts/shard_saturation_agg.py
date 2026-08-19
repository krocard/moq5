#!/usr/bin/env python3
"""Aggregate raw bench_relay_saturation rows into ceiling curves and open-loop
knees. MANUAL analysis tool (never CI wall-clock gating): it consumes the CSV
the bench emits, drops every row that is not a clean canonical measurement, and
produces median/min/max summaries plus the rate-dependent knee lambda*(K) with a
deterministic bootstrap confidence interval.

Row admission (a row must be ALL of):
  * valid == 1                         (every exact-total/quiescence gate held)
  * remote_data_rejected == 0          (no loss)
  * term_capacity == 0, term_overrun == 0
  * pinned == 1                        unless --allow-unpinned (exploratory only)
  * ceiling rows: backlog_emptied == 0 (starved rows carry no B*), campaign==ceiling
  * knee rows:    campaign == knee     (generator validity already enforced by
                                        the bench: an invalid knee row has valid==0)
  * every consumed numeric value finite (a NaN/Inf cell is a malformed row)

Ceiling rows are grouped by the FULL workload key (K, shape, size, fanout,
distribution, CAP) — rows at different CAPs are different operating points and
never pool into one median. Per curve point (all dims except CAP) the ladder is
then CALIBRATED: ascending CAP, each rung needs >= --min-reps valid reps, and the
selected rung is the first whose median B* the next rung does not raise by more
than PLATEAU_TOL (5%) — the explicit plateau. A point with no plateau (B* still
rising at the largest CAP, or under-replicated rungs) reports none and never
enters the canonical curve.

Knee: lambda*(K) is the first generator-valid offered rate (ascending in the
ACTUAL offered rate) whose median completion rate mu < (1-epsilon) * median
actual offered rate, epsilon default 0.10 (must satisfy 0 < epsilon < 1). The
bootstrap resamples reps with a FIXED seed so the CI and the median/min/max are
byte-reproducible.

Unknown flags and malformed/short rows are hard errors.
"""

import math
import sys

EPSILON_DEFAULT = 0.10
MIN_REPS_DEFAULT = 7
PLATEAU_TOL = 0.05
BOOTSTRAP_SAMPLES = 512
BOOTSTRAP_SEED = 0x5A7 * 1000 + 3  # fixed; recorded in the summary


def die(msg):
    sys.stderr.write("shard_saturation_agg: %s\n" % msg)
    sys.exit(2)


# --- a tiny deterministic PRNG (xorshift64) so results never depend on the
# platform's random module version ---
class Rng:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFFFFFFFFFF or 1

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 7
        x ^= (x << 17) & 0xFFFFFFFFFFFFFFFF
        self.s = x
        return x

    def below(self, n):
        return self.next() % n


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    if n % 2:
        return float(s[n // 2])
    return (s[n // 2 - 1] + s[n // 2]) / 2.0


def parse_rows(text):
    lines = [ln for ln in text.splitlines() if ln.strip() != ""]
    if not lines:
        return []
    header = lines[0].split(",")
    idx = {name: i for i, name in enumerate(header)}
    required = ["campaign", "valid", "shards", "object_shape", "object_size",
                "fanout", "distribution", "cap",
                "wall_objects_per_sec", "actual_offered_rate",
                "B_star_msgs_per_sec", "mu_cap", "backlog_emptied", "pinned",
                "remote_data_rejected", "term_capacity", "term_overrun",
                "requested_lambda"]
    for r in required:
        if r not in idx:
            die("missing required column '%s' in header" % r)
    rows = []
    for lineno, ln in enumerate(lines[1:], start=2):
        cells = ln.split(",")
        if len(cells) != len(header):
            die("row %d has %d cells, header has %d" %
                (lineno, len(cells), len(header)))
        rows.append({name: cells[idx[name]] for name in idx})
    return rows


def as_int(v):
    return None if v == "null" else int(v)


def as_float(v):
    if v == "null":
        return None
    f = float(v)
    if not math.isfinite(f):
        die("non-finite numeric value '%s' in a row" % v)
    return f


def admit(row, allow_unpinned):
    if as_int(row["valid"]) != 1:
        return "invalid"
    if as_int(row["remote_data_rejected"]) != 0:
        return "lossy"
    if as_int(row["term_capacity"]) != 0 or as_int(row["term_overrun"]) != 0:
        return "fail-stop"
    if not allow_unpinned and as_int(row["pinned"]) != 1:
        return "unpinned"
    return None


def group_key(row):
    """Full workload key EXCLUDING CAP (one curve point)."""
    return (row["shards"], row["object_shape"], row["object_size"],
            row["fanout"], row["distribution"])


def cap_of(row):
    return int(row["cap"])


def aggregate_ceiling(rows, allow_unpinned, min_reps):
    """Per curve point: group rows by (workload key, CAP), require min_reps valid
    reps per CAP rung, then select the first explicit plateau rung (the first CAP
    whose median B* the next rung does not raise by more than PLATEAU_TOL). Rows
    at different CAPs NEVER pool into one median. EVERY observed rung is
    registered — including one whose rows are all invalid — so a hole in the
    ladder is visible and never bridged."""
    groups = {}
    rejects = {}
    for row in rows:
        if row["campaign"] != "ceiling":
            continue
        # Register the rung FIRST (even if this row is then rejected): a rung
        # whose rows are all invalid must still appear as a hole in the ladder.
        # Per-rung failure kinds are retained because only STARVATION is an
        # expected pre-saturation outcome — any other failure on a leading rung
        # blocks calibration.
        key = group_key(row)
        cap = cap_of(row)
        groups.setdefault(key, {}).setdefault(
            cap, {"bstar": [], "mu": [], "starved": 0, "bad": 0})
        why = admit(row, allow_unpinned)
        if why is None and as_int(row["backlog_emptied"]) != 0:
            why = "starved"
        if why is not None:
            rejects[why] = rejects.get(why, 0) + 1
            if why == "starved":
                groups[key][cap]["starved"] += 1
            else:
                groups[key][cap]["bad"] += 1
            continue
        bstar = as_float(row["B_star_msgs_per_sec"])
        mu = as_float(row["mu_cap"])
        if bstar is None or mu is None:
            rejects["no-ceiling"] = rejects.get("no-ceiling", 0) + 1
            groups[key][cap]["bad"] += 1
            continue
        groups[key][cap]["bstar"].append(bstar)
        groups[key][cap]["mu"].append(mu)
    out = []
    for key in sorted(groups):
        rungs = groups[key]
        caps = sorted(rungs)
        under = [c for c in caps if len(rungs[c]["bstar"]) < min_reps]
        if under:
            rejects["under-replicated-rung"] = \
                rejects.get("under-replicated-rung", 0) + len(under)
        # Plateau: two ADJACENT observed rungs, BOTH sufficiently replicated,
        # whose medians agree within PLATEAU_TOL in ABSOLUTE relative change (a
        # falling curve is a regime anomaly, not a plateau). A LEADING rung may
        # be skipped ONLY when it failed purely by STARVATION (zero valid rows,
        # zero other failures — the expected shape of an under-CAP rung below
        # saturation). A leading rung with any invalid/lossy/unpinned/fail-stop/
        # missing-ceiling row, or with partial valid replication, blocks
        # calibration outright; and once usable measurements begin, any broken
        # rung is an unmeasured hole that BREAKS the curve — never bridged.
        i0 = 0
        blocked = False
        while i0 < len(caps) and len(rungs[caps[i0]]["bstar"]) < min_reps:
            r0 = rungs[caps[i0]]
            if not (len(r0["bstar"]) == 0 and r0["bad"] == 0 and
                    r0["starved"] > 0):
                blocked = True   # a leading failure that is NOT pure starvation
                break
            i0 += 1
        plateau = None
        for i in ([] if blocked else range(i0, len(caps) - 1)):
            c0, c1 = caps[i], caps[i + 1]
            if len(rungs[c0]["bstar"]) < min_reps or \
               len(rungs[c1]["bstar"]) < min_reps:
                break   # hole after measurements began: the ladder ends here
            lo = median(rungs[c0]["bstar"])
            hi = median(rungs[c1]["bstar"])
            if lo > 0 and abs(hi - lo) <= PLATEAU_TOL * lo:
                plateau = c0
                break
        entry = {
            "shards": key[0], "object_shape": key[1], "object_size": key[2],
            "fanout": key[3], "distribution": key[4],
            "caps_tested": len(caps), "plateau_cap": plateau,
        }
        if plateau is not None:
            g = rungs[plateau]
            entry.update({
                "reps": len(g["bstar"]),
                "b_star_med": median(g["bstar"]), "b_star_min": min(g["bstar"]),
                "b_star_max": max(g["bstar"]),
                "mu_cap_med": median(g["mu"]), "mu_cap_min": min(g["mu"]),
                "mu_cap_max": max(g["mu"]),
            })
        out.append(entry)
    return out, rejects


def knee_breakpoint(points, epsilon):
    """points: list of (actual_rate, mu) for ONE rep set at ascending lambda.
    Returns the first actual_rate where mu < (1-epsilon)*actual_rate, else None."""
    for rate, mu in sorted(points):
        if rate > 0 and mu < (1.0 - epsilon) * rate:
            return rate
    return None


def detect_knee(rows, epsilon, allow_unpinned, seed, samples, min_reps):
    groups = {}
    rejects = {}
    for row in rows:
        if row["campaign"] != "knee":
            continue
        # Register the lambda rung FIRST (requested_lambda parses on every knee
        # row): a wholly-invalid rate must remain visible as a hole in the
        # sweep, not silently vanish so its neighbors look adjacent.
        req = as_float(row["requested_lambda"])
        if req is not None:
            groups.setdefault(group_key(row), {}).setdefault(req, [])
        why = admit(row, allow_unpinned)
        if why is not None:
            rejects[why] = rejects.get(why, 0) + 1
            continue
        rate = as_float(row["actual_offered_rate"])
        mu = as_float(row["wall_objects_per_sec"])
        if rate is None or mu is None or req is None:
            rejects["not-knee-fields"] = rejects.get("not-knee-fields", 0) + 1
            continue
        groups[group_key(row)][req].append((rate, mu))
    out = []
    for key in sorted(groups):
        by_lambda = groups[key]
        # Every lambda rung must carry at least min_reps valid reps: a knee
        # established by a lone surviving row is not a measurement. And the knee
        # claim ("the FIRST rate where mu departs lambda") is only sound if
        # every lower rate was actually measured — so the scan covers ONLY the
        # contiguous usable prefix of the observed sweep; a broken rung
        # (under-replicated or wholly invalid) ends it, never skipped over.
        under = [lam for lam in by_lambda if len(by_lambda[lam]) < min_reps]
        if under:
            rejects["under-replicated-rung"] = \
                rejects.get("under-replicated-rung", 0) + len(under)
        lambdas = []
        for lam in sorted(by_lambda):
            if len(by_lambda[lam]) < min_reps:
                break   # hole: nothing past it may support a knee claim
            lambdas.append(lam)
        if not lambdas:
            continue
        # median curve
        med_points = []
        for lam in lambdas:
            reps = by_lambda[lam]
            med_points.append((median([r for r, _ in reps]),
                               median([m for _, m in reps])))
        knee = knee_breakpoint(med_points, epsilon)
        # Deterministic bootstrap over reps at each lambda; report the MINIMUM
        # rep count across surviving rungs (never the maximum). A resample with
        # NO detected knee is a RIGHT-CENSORED outcome (the knee lies past the
        # swept range in that resample), not a discardable one: the detection
        # fraction is reported, percentiles are taken over ALL samples with
        # censored outcomes sorting last (+inf), and a percentile landing in the
        # censored region yields an OPEN bound rather than a fabricated number.
        rng = Rng(seed)
        finite = []
        nrep = min(len(by_lambda[lam]) for lam in lambdas)
        for _ in range(samples):
            pts = []
            for lam in lambdas:
                reps = by_lambda[lam]
                # resample this lambda's reps with replacement
                rs = [reps[rng.below(len(reps))] for _ in reps]
                pts.append((median([r for r, _ in rs]),
                            median([m for _, m in rs])))
            bp = knee_breakpoint(pts, epsilon)
            if bp is not None:
                finite.append(bp)
        ndet = len(finite)
        detect_frac = float(ndet) / float(samples) if samples > 0 else 0.0
        ci = None
        if ndet > 0:
            finite.sort()
            lo_idx = int(0.025 * (samples - 1))
            hi_idx = int(0.975 * (samples - 1))
            lo = finite[lo_idx] if lo_idx < ndet else None   # censored: open
            hi = finite[hi_idx] if hi_idx < ndet else None   # censored: open
            ci = (lo, hi)
        out.append({
            "shards": key[0], "object_shape": key[1], "object_size": key[2],
            "fanout": key[3], "distribution": key[4],
            "lambda_points": len(lambdas), "reps_per_point": nrep,
            "knee_lambda": knee, "ci": ci, "detect_frac": detect_frac,
            "bootstrap_seed": seed, "bootstrap_samples": samples,
        })
    return out, rejects


def emit_summary(ceiling, cej, knee, knj, epsilon, min_reps):
    print("# shard saturation aggregation (in_process_wall)")
    print("# epsilon=%.2f min_reps=%d plateau_tol=%.2f bootstrap_seed=%d "
          "bootstrap_samples=%d" %
          (epsilon, min_reps, PLATEAU_TOL, BOOTSTRAP_SEED, BOOTSTRAP_SAMPLES))
    print("## ceiling (closed-loop): B*(K), mu_cap(K) at the calibrated CAP")
    print("shards,object_shape,object_size,fanout,distribution,caps_tested,"
          "plateau_cap,reps,b_star_med,b_star_min,b_star_max,mu_cap_med,"
          "mu_cap_min,mu_cap_max")
    for c in ceiling:
        if c["plateau_cap"] is None:
            print("%s,%s,%s,%s,%s,%d,none,,,,,,," %
                  (c["shards"], c["object_shape"], c["object_size"],
                   c["fanout"], c["distribution"], c["caps_tested"]))
            continue
        print("%s,%s,%s,%s,%s,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f" %
              (c["shards"], c["object_shape"], c["object_size"], c["fanout"],
               c["distribution"], c["caps_tested"], c["plateau_cap"], c["reps"],
               c["b_star_med"], c["b_star_min"], c["b_star_max"],
               c["mu_cap_med"], c["mu_cap_min"], c["mu_cap_max"]))
    if cej:
        print("# ceiling rejects: " +
              ", ".join("%s=%d" % (k, v) for k, v in sorted(cej.items())))
    print("## knee (open-loop): lambda*(K)")
    print("shards,object_shape,object_size,fanout,distribution,lambda_points,"
          "reps_per_point,knee_lambda,knee_detect_frac,ci_lo,ci_hi")
    for k in knee:
        kl = "none" if k["knee_lambda"] is None else "%.1f" % k["knee_lambda"]

        def bound(v):
            # None inside a CI tuple == right-censored percentile: OPEN bound.
            return "open" if v is None else "%.1f" % v

        clo = "none" if k["ci"] is None else bound(k["ci"][0])
        chi = "none" if k["ci"] is None else bound(k["ci"][1])
        print("%s,%s,%s,%s,%s,%d,%d,%s,%.3f,%s,%s" %
              (k["shards"], k["object_shape"], k["object_size"], k["fanout"],
               k["distribution"], k["lambda_points"], k["reps_per_point"],
               kl, k["detect_frac"], clo, chi))
    if knj:
        print("# knee rejects: " +
              ", ".join("%s=%d" % (k, v) for k, v in sorted(knj.items())))


# --------------------------------------------------------------- selftest
HDR = ("campaign,valid,shards,object_shape,object_size,fanout,distribution,"
       "cap,wall_objects_per_sec,actual_offered_rate,B_star_msgs_per_sec,"
       "mu_cap,backlog_emptied,pinned,remote_data_rejected,term_capacity,"
       "term_overrun,requested_lambda")


def _line(campaign, valid="1", shards="4", mu="0", rate="null", bstar="null",
          mucap="null", starved="0", pinned="1", loss="0", req="null",
          cap="128"):
    return ",".join([campaign, valid, shards, "whole", "257", "1",
                     "remote-uniform", cap, mu, rate, bstar, mucap, starved,
                     pinned, loss, "0", "0", req])


def selftest():
    fails = 0

    # (a) knee detector accepts a synthetic tracks-then-plateaus curve.
    # mu tracks the offered rate up to 4000, then plateaus at ~5000: the knee is
    # the first rate where mu < 0.9*rate. At rate 6000 mu=5000 (<5400) -> knee.
    lines = [HDR]
    curve = [(1000, 1000), (2000, 2000), (4000, 3950), (6000, 5000),
             (8000, 5050)]
    for rate, mu in curve:
        for _ in range(7):
            lines.append(_line("knee", mu=str(mu), rate=str(rate),
                               req=str(rate)))
    rows = parse_rows("\n".join(lines))
    knee, _ = detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                          BOOTSTRAP_SAMPLES, 7)
    if not (len(knee) == 1 and knee[0]["knee_lambda"] == 6000.0):
        sys.stderr.write("FAIL: knee detect got %r (want 6000)\n" % knee)
        fails += 1
    else:
        print("PASS: knee detector finds tracks-then-plateaus knee at 6000")

    # (b) invalid / lossy / unpinned knee rows are excluded.
    lines = [HDR]
    lines.append(_line("knee", valid="0", mu="1000", rate="6000", req="6000"))
    lines.append(_line("knee", loss="3", mu="1000", rate="6000", req="6000"))
    lines.append(_line("knee", pinned="0", mu="1000", rate="6000", req="6000"))
    rows = parse_rows("\n".join(lines))
    _, rej = detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                         BOOTSTRAP_SAMPLES, 7)
    if rej.get("invalid") == 1 and rej.get("lossy") == 1 and \
       rej.get("unpinned") == 1:
        print("PASS: knee excludes invalid/lossy/unpinned rows")
    else:
        sys.stderr.write("FAIL: knee rejects = %r\n" % rej)
        fails += 1

    # (c) ceiling aggregation excludes starved rows (min_reps=1 keeps this case
    # focused on the starved-row rejection, not replication).
    lines = [HDR]
    for _ in range(2):
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="128"))
        lines.append(_line("ceiling", bstar="605000", mucap="505000",
                           cap="256"))
    lines.append(_line("ceiling", starved="1", cap="128"))
    rows = parse_rows("\n".join(lines))
    cel, cej = aggregate_ceiling(rows, False, 1)
    if len(cel) == 1 and cel[0]["plateau_cap"] == 128 and \
       cel[0]["reps"] == 2 and cej.get("starved") == 1:
        print("PASS: ceiling aggregation excludes starved rows")
    else:
        sys.stderr.write("FAIL: ceiling=%r rejects=%r\n" % (cel, cej))
        fails += 1

    # (f) mixed CAPs never pool into one median: two rungs with very different
    # medians stay separate; the plateau selects the first rung the next one
    # does not raise by more than PLATEAU_TOL. Rung ladder: 32 -> 300k,
    # 128 -> 600k (rising, NOT a plateau at 32), 256 -> 610k (plateau at 128).
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("ceiling", bstar="300000", mucap="250000", cap="32"))
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="128"))
        lines.append(_line("ceiling", bstar="610000", mucap="505000",
                           cap="256"))
    rows = parse_rows("\n".join(lines))
    cel, _ = aggregate_ceiling(rows, False, 7)
    if len(cel) == 1 and cel[0]["plateau_cap"] == 128 and \
       cel[0]["b_star_med"] == 600000.0 and cel[0]["caps_tested"] == 3:
        print("PASS: mixed CAPs not pooled; plateau selects CAP=128 "
              "(B*=600000, not a 32/128/256 blend)")
    else:
        sys.stderr.write("FAIL: mixed-CAP result %r\n" % cel)
        fails += 1

    # (g) an under-replicated rung cannot join plateau detection: with
    # min_reps=7 a 3-rep rung is dropped, leaving one usable rung -> no plateau.
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="128"))
    for _ in range(3):
        lines.append(_line("ceiling", bstar="610000", mucap="505000",
                           cap="256"))
    rows = parse_rows("\n".join(lines))
    cel, cej = aggregate_ceiling(rows, False, 7)
    if len(cel) == 1 and cel[0]["plateau_cap"] is None and \
       cej.get("under-replicated-rung") == 1:
        print("PASS: under-replicated rung dropped; no plateau claimed")
    else:
        sys.stderr.write("FAIL: under-replication %r rejects=%r\n" % (cel, cej))
        fails += 1

    # (h) a FALLING curve is not a plateau: 600k -> 300k is a regime anomaly
    # (|change| way past tolerance), so no CAP is declared calibrated.
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="128"))
        lines.append(_line("ceiling", bstar="300000", mucap="250000",
                           cap="256"))
    rows = parse_rows("\n".join(lines))
    cel, _ = aggregate_ceiling(rows, False, 7)
    if len(cel) == 1 and cel[0]["plateau_cap"] is None:
        print("PASS: falling curve is not a plateau (600k->300k -> none)")
    else:
        sys.stderr.write("FAIL: falling-curve result %r\n" % cel)
        fails += 1

    # (i) an under-replicated MIDDLE rung breaks the ladder: 32(7 reps) /
    # 128(1 rep) / 256(7 reps) has no adjacent fully-replicated pair, so no
    # plateau may be claimed by bridging 32 and 256.
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="32"))
        lines.append(_line("ceiling", bstar="610000", mucap="505000",
                           cap="256"))
    lines.append(_line("ceiling", bstar="605000", mucap="502000", cap="128"))
    rows = parse_rows("\n".join(lines))
    cel, cej = aggregate_ceiling(rows, False, 7)
    if len(cel) == 1 and cel[0]["plateau_cap"] is None and \
       cej.get("under-replicated-rung") == 1:
        print("PASS: broken middle rung never bridged (32/[128]/256 -> none)")
    else:
        sys.stderr.write("FAIL: middle-rung result %r rejects=%r\n" %
                         (cel, cej))
        fails += 1

    # (k) a wholly-invalid ceiling rung is a registered HOLE, never bridged:
    # 32 (7 valid) / 128 (all rows invalid) / 256 (7 valid) has no adjacent
    # usable pair once measurements began -> no plateau.
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="32"))
        lines.append(_line("ceiling", valid="0", cap="128"))
        lines.append(_line("ceiling", bstar="610000", mucap="505000",
                           cap="256"))
    rows = parse_rows("\n".join(lines))
    cel, cej = aggregate_ceiling(rows, False, 7)
    if len(cel) == 1 and cel[0]["plateau_cap"] is None and \
       cel[0]["caps_tested"] == 3:
        print("PASS: wholly-invalid ceiling rung registered as a hole "
              "(32/[128 invalid]/256 -> none)")
    else:
        sys.stderr.write("FAIL: invalid-rung hole %r rejects=%r\n" %
                         (cel, cej))
        fails += 1

    # (l) LEADING broken ceiling rungs may be skipped (an under-CAP rung only
    # starves before saturation): 32 (all starved) / 128 / 256 -> plateau 128.
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("ceiling", starved="1", cap="32"))
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="128"))
        lines.append(_line("ceiling", bstar="610000", mucap="505000",
                           cap="256"))
    rows = parse_rows("\n".join(lines))
    cel, _ = aggregate_ceiling(rows, False, 7)
    if len(cel) == 1 and cel[0]["plateau_cap"] == 128 and \
       cel[0]["b_star_med"] == 600000.0:
        print("PASS: leading starved CAP skipped; plateau found above it")
    else:
        sys.stderr.write("FAIL: leading-starved result %r\n" % cel)
        fails += 1

    # (n) a LEADING rung that failed for any reason OTHER than starvation
    # blocks calibration entirely: 32 (all rows invalid) / healthy 128 / 256
    # must NOT report a calibrated plateau (only pure starvation is the
    # expected pre-saturation shape).
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("ceiling", valid="0", cap="32"))
        lines.append(_line("ceiling", bstar="600000", mucap="500000",
                           cap="128"))
        lines.append(_line("ceiling", bstar="610000", mucap="505000",
                           cap="256"))
    rows = parse_rows("\n".join(lines))
    cel, _ = aggregate_ceiling(rows, False, 7)
    if len(cel) == 1 and cel[0]["plateau_cap"] is None:
        print("PASS: leading all-invalid rung blocks calibration "
              "(not skippable like starvation)")
    else:
        sys.stderr.write("FAIL: leading-invalid result %r\n" % cel)
        fails += 1

    # (o) mixed-support bootstrap: at the knee rung the reps straddle the
    # detection threshold, so only a fraction of resamples find a knee. The
    # no-knee resamples are right-censored: the detection fraction is
    # reported and the 97.5th percentile lands in the censored region -> the
    # upper bound is OPEN, never fabricated from the detected subset alone.
    lines = [HDR]
    for rate in (1000, 2000, 4000):
        for _ in range(7):
            lines.append(_line("knee", mu=str(int(rate * 0.99)),
                               rate=str(rate), req=str(rate)))
    for mu in (5000, 5000, 5000, 5000, 5500, 5500, 5500):
        lines.append(_line("knee", mu=str(mu), rate="6000", req="6000"))
    rows = parse_rows("\n".join(lines))
    kn, _ = detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                        BOOTSTRAP_SAMPLES, 7)
    if len(kn) == 1 and kn[0]["knee_lambda"] == 6000.0 and \
       0.0 < kn[0]["detect_frac"] < 1.0 and kn[0]["ci"] is not None and \
       kn[0]["ci"][0] == 6000.0 and kn[0]["ci"][1] is None:
        print("PASS: mixed-support bootstrap right-censored "
              "(detect_frac=%.3f, ci_hi open)" % kn[0]["detect_frac"])
    else:
        sys.stderr.write("FAIL: mixed-support bootstrap %r\n" % kn)
        fails += 1

    # (m) a wholly-invalid lambda rung ends the knee scan: 1000 (valid,
    # tracking) / 2000 (all invalid) / 4000 (valid, knee-worthy) must NOT claim
    # a knee at 4000 across the unmeasured hole.
    lines = [HDR]
    for _ in range(7):
        lines.append(_line("knee", mu="1000", rate="1000", req="1000"))
        lines.append(_line("knee", valid="0", mu="1800", rate="2000",
                           req="2000"))
        lines.append(_line("knee", mu="2000", rate="4000", req="4000"))
    rows = parse_rows("\n".join(lines))
    kn, rej = detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                          BOOTSTRAP_SAMPLES, 7)
    if len(kn) == 1 and kn[0]["knee_lambda"] is None and \
       kn[0]["lambda_points"] == 1 and \
       rej.get("under-replicated-rung", 0) >= 1:
        print("PASS: wholly-invalid lambda rung ends the knee scan "
              "(no knee claimed across the hole)")
    else:
        sys.stderr.write("FAIL: knee-hole result %r rejects=%r\n" % (kn, rej))
        fails += 1

    # (j) knee rungs enforce min-reps: a single-row lambda rung is excluded
    # (counted), and reps_per_point reports the MINIMUM surviving count.
    lines = [HDR]
    curve = [(1000, 1000), (2000, 2000), (4000, 3950), (6000, 5000)]
    for rate, mu in curve:
        for _ in range(7):
            lines.append(_line("knee", mu=str(mu), rate=str(rate),
                               req=str(rate)))
    lines.append(_line("knee", mu="100", rate="12000", req="12000"))  # 1 rep
    rows = parse_rows("\n".join(lines))
    kn, rej = detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                          BOOTSTRAP_SAMPLES, 7)
    if len(kn) == 1 and kn[0]["knee_lambda"] == 6000.0 and \
       kn[0]["lambda_points"] == 4 and kn[0]["reps_per_point"] == 7 and \
       rej.get("under-replicated-rung") == 1:
        print("PASS: knee excludes under-replicated lambda rung; "
              "reps_per_point is the minimum")
    else:
        sys.stderr.write("FAIL: knee min-reps %r rejects=%r\n" % (kn, rej))
        fails += 1

    # (d) bootstrap + median/min/max output is deterministic (same twice).
    lines = [HDR]
    curve = [(1000, 1000), (2000, 2000), (4000, 3950), (6000, 5000)]
    for rate, mu in curve:
        for _ in range(7):
            lines.append(_line("knee", shards="8", mu=str(mu), rate=str(rate),
                               req=str(rate)))
    rows = parse_rows("\n".join(lines))
    k1, _ = detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                        BOOTSTRAP_SAMPLES, 7)
    k2, _ = detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                        BOOTSTRAP_SAMPLES, 7)
    if k1 == k2 and k1 and k1[0]["ci"] is not None and \
       k1[0]["detect_frac"] == 1.0 and None not in k1[0]["ci"]:
        print("PASS: bootstrap + aggregation deterministic (CI=%r, "
              "full support)" % (k1[0]["ci"],))
    else:
        sys.stderr.write("FAIL: nondeterministic bootstrap %r vs %r\n" %
                         (k1, k2))
        fails += 1

    # (e) malformed rows are hard errors: short rows and non-finite values.
    try:
        parse_rows(HDR + "\nknee,1,4,whole")  # short row
        sys.stderr.write("FAIL: short row not rejected\n")
        fails += 1
    except SystemExit:
        print("PASS: malformed (short) row rejected")
    try:
        rows = parse_rows("\n".join([HDR, _line("knee", mu="nan", rate="6000",
                                                req="6000")]))
        detect_knee(rows, EPSILON_DEFAULT, False, BOOTSTRAP_SEED,
                    BOOTSTRAP_SAMPLES, 7)
        sys.stderr.write("FAIL: NaN row value not rejected\n")
        fails += 1
    except SystemExit:
        print("PASS: non-finite row value rejected")

    if fails == 0:
        print("# ALL PASS")
    return 1 if fails else 0


def main(argv):
    epsilon = EPSILON_DEFAULT
    min_reps = MIN_REPS_DEFAULT
    allow_unpinned = False
    do_selftest = False
    path = None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--selftest":
            do_selftest = True
        elif a == "--allow-unpinned":
            allow_unpinned = True
        elif a == "--epsilon":
            i += 1
            if i >= len(argv):
                die("--epsilon needs a value")
            try:
                epsilon = float(argv[i])
            except ValueError:
                die("--epsilon not a number: %s" % argv[i])
            if not math.isfinite(epsilon) or not (0.0 < epsilon < 1.0):
                die("--epsilon must satisfy 0 < epsilon < 1 (got %s)" % argv[i])
        elif a == "--min-reps":
            i += 1
            if i >= len(argv):
                die("--min-reps needs a value")
            try:
                min_reps = int(argv[i])
            except ValueError:
                die("--min-reps not an integer: %s" % argv[i])
            if min_reps < 1:
                die("--min-reps must be >= 1")
        elif a == "--input":
            i += 1
            if i >= len(argv):
                die("--input needs a path")
            path = argv[i]
        elif a.startswith("--"):
            die("unknown flag: %s" % a)
        else:
            path = a
        i += 1

    if do_selftest:
        return selftest()

    text = sys.stdin.read() if path in (None, "-") else open(path).read()
    rows = parse_rows(text)
    ceiling, cej = aggregate_ceiling(rows, allow_unpinned, min_reps)
    knee, knj = detect_knee(rows, epsilon, allow_unpinned, BOOTSTRAP_SEED,
                            BOOTSTRAP_SAMPLES, min_reps)
    emit_summary(ceiling, cej, knee, knj, epsilon, min_reps)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
