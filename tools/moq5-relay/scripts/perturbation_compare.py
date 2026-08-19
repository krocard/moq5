#!/usr/bin/env python3
"""Two-binary perturbation comparator (blast-report JSON only).

Decides whether an instrumented relay binary perturbs performance relative
to a pre-instrumentation binary, from randomized interleaved blocks: each
block ran ONE old-binary rep and ONE new-binary rep back to back. This tool
never parses relay logs (the old binary emits none of the strict records),
so the production paired-log parser stays strict and untouched.

Pairing and inference: the pair is the within-block delta; a deterministic
block bootstrap (fixed seed, 512 resamples) gives the 95% CI of the median
relative delta for wall_objects_per_sec and latency_p99_ns. The gate PASSES
only when every block is complete and valid AND each CI excludes a
regression worse than --tolerance (default 2%). Incomplete blocks fail the
gate closed — there is no survivor-subset CI.

Usage:
  perturbation_compare.py --blocks N --tolerance 0.02 \
      --block <old.json> <new.json>  (repeated, once per block)
  perturbation_compare.py --selftest
"""

import json
import math
import sys

RESAMPLES = 512
SEED = 0x5A7 * 1000 + 11


def die(msg):
    sys.stderr.write("perturbation_compare: %s\n" % msg)
    sys.exit(2)


class Rng(object):
    """xorshift64 — deterministic across runs and platforms."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFFFFFFFFFF or 1

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFFFFFFFFFF
        s ^= s >> 7
        s ^= (s << 17) & 0xFFFFFFFFFFFFFFFF
        self.s = s
        return s

    def below(self, n):
        return self.next() % n


def median(xs):
    ys = sorted(xs)
    n = len(ys)
    return ys[n // 2] if n % 2 else (ys[n // 2 - 1] + ys[n // 2]) / 2.0


def _finite_pos(v):
    """A finite positive JSON number — booleans are numbers to json/
    isinstance and must be rejected explicitly."""
    return (not isinstance(v, bool) and isinstance(v, (int, float)) and
            math.isfinite(v) and v > 0)


FP_FIELDS = ("alpn", "namespace_count", "tracks_per_namespace",
             "publishers_per_namespace", "subscribers_per_track",
             "groups", "objects_per_group", "first_object_size",
             "other_object_size")


def _count(v):
    """A nonnegative integer count — booleans excluded (json decodes them
    as ints for isinstance)."""
    return isinstance(v, int) and not isinstance(v, bool) and v >= 0


def load_metrics(path, text=None):
    """A passed blast report's two decision metrics PLUS its semantic
    fingerprint. Correctness must have passed; the basis must be
    relay_e2e_wall; metrics must be finite positive non-boolean numbers.
    The caller requires ONE identical fingerprint across every old/new
    report — two different workloads (or measurement classes) must never
    meet in one CI."""
    try:
        raw = text if text is not None else open(path).read()
        d = json.loads(raw)
        if d["correctness"]["passed"] is not True:
            return None, "correctness failed in %s" % path
        m = d["metrics"]
        if m["measurement_basis"] != "relay_e2e_wall":
            return None, "measurement_basis %r != relay_e2e_wall in %s" % (
                m["measurement_basis"], path)
        rate = m["wall_objects_per_sec"]
        p99 = m["latency_p99_ns"]
        fair = d["fairness"]
        if not (isinstance(fair["alpn"], str) and fair["alpn"]):
            return None, "alpn must be a nonempty string in %s" % path
        for k in FP_FIELDS[1:]:
            if not _count(fair[k]):
                return None, "%s must be a nonnegative integer in %s" % (
                    k, path)
        expected = (fair["namespace_count"] * fair["tracks_per_namespace"] *
                    fair["subscribers_per_track"] * fair["groups"] *
                    fair["objects_per_group"])
        if expected == 0:
            return None, "zero expected deliveries in %s" % path
        notes = [n for n in d.get("ergonomics_notes", [])
                 if isinstance(n, str) and n.startswith("retrieval=")]
        if len(notes) != 1:
            return None, "%d retrieval notes (need exactly one) in %s" % (
                len(notes), path)
        mode = notes[0][len("retrieval="):].split(" ", 1)[0]
        if mode not in ("subscribe", "fetch"):
            return None, "unknown retrieval mode %r in %s" % (mode, path)
        impl = d.get("impl")
        if not (isinstance(impl, str) and impl):
            return None, "impl must be a nonempty string in %s" % path
        fp = tuple((k, fair[k]) for k in FP_FIELDS) + \
            (("retrieval", mode), ("impl", impl))
    except (OSError, ValueError, KeyError, TypeError) as e:
        return None, "unreadable/malformed report %s: %s" % (path, e)
    if not (_finite_pos(rate) and _finite_pos(p99)):
        return None, "non-finite/non-positive/boolean metric in %s" % path
    return {"rate": float(rate), "p99": float(p99), "fp": fp}, None


def bootstrap_ci(deltas):
    """95% CI of the median via the deterministic block bootstrap."""
    rng = Rng(SEED)
    meds = []
    n = len(deltas)
    for _ in range(RESAMPLES):
        meds.append(median([deltas[rng.below(n)] for _ in range(n)]))
    meds.sort()
    lo = meds[int(0.025 * RESAMPLES)]
    hi = meds[int(0.975 * RESAMPLES) - 1]
    return lo, hi


EXPECT_FP = {"alpn": "moqt-16", "retrieval": "subscribe",
             "namespace_count": 1, "tracks_per_namespace": 1,
             "publishers_per_namespace": 1, "subscribers_per_track": 16,
             "groups": 16, "objects_per_group": 64,
             "first_object_size": 512, "other_object_size": 512}


def decide(blocks, expect_blocks, tolerance, expect_fp=None,
           require_improvement=False, min_gain=None):
    """blocks = [(old_metrics, new_metrics)] — one tuple per COMPLETE
    block. Returns (verdict_bool, lines). Fails closed on block count, on
    mixed fingerprints, AND on a fingerprint that is not the gate's FIXED
    operating point (a drifted workload on BOTH binaries is otherwise
    invisible to the identity check)."""
    lines = []
    if expect_fp is None:
        expect_fp = EXPECT_FP
    fps = {m["fp"] for b in blocks for m in b if "fp" in m}
    if len(fps) > 1:
        return False, ["FAIL: %d distinct workload fingerprints across the "
                       "old/new reports — different workloads or classes "
                       "must never meet in one CI" % len(fps)]
    if len(fps) == 1:
        d = dict(next(iter(fps)))
        wrong = [k for k, v in expect_fp.items() if d.get(k) != v]
        if wrong:
            return False, ["FAIL: workload is not the gate's fixed "
                           "operating point (mismatched: %s)"
                           % ", ".join(sorted(wrong))]
    if len(blocks) != expect_blocks:
        return False, ["FAIL: %d complete blocks != expected %d — the "
                       "operating point publishes no CI (fail closed)"
                       % (len(blocks), expect_blocks)]
    # relative delta, new vs old: positive = new is worse for p99,
    # negative = new is worse for rate
    rate_d = [(b[1]["rate"] - b[0]["rate"]) / b[0]["rate"] for b in blocks]
    p99_d = [(b[1]["p99"] - b[0]["p99"]) / b[0]["p99"] for b in blocks]
    ok = True
    for name, deltas, worse_low in (("rate", rate_d, True),
                                    ("p99", p99_d, False)):
        lo, hi = bootstrap_ci(deltas)
        med = median(deltas)
        # regression = rate lower than -tol, or p99 higher than +tol;
        # the CI must EXCLUDE the regression region
        if worse_low:
            good = lo > -tolerance
        else:
            good = hi < tolerance
        lines.append("%s: median=%+.4f ci95=[%+.4f,%+.4f] tol=%.4f -> %s"
                     % (name, med, lo, hi, tolerance,
                        "OK" if good else "REGRESSION-NOT-EXCLUDED"))
        ok = ok and good
        if name == "rate":
            # Signed gain gates (rate only; positive delta = faster):
            # improvement is the CI95 LOWER BOUND above zero — a positive
            # median whose CI crosses zero proves nothing — and the minimum
            # meaningful gain is a MEDIAN threshold on top of significance.
            if require_improvement:
                good_imp = lo > 0.0
                lines.append("rate improvement: ci95-lo=%+.4f -> %s"
                             % (lo, "SIGNIFICANT" if good_imp
                                else "NOT-SIGNIFICANT (CI crosses zero)"))
                ok = ok and good_imp
            if min_gain is not None:
                good_gain = med >= min_gain
                lines.append("rate minimum gain: median=%+.4f needed=%+.4f "
                             "-> %s" % (med, min_gain,
                                        "MET" if good_gain else "BELOW-MINIMUM"))
                ok = ok and good_gain
    lines.append("PERTURBATION GATE: %s" % ("PASS" if ok else "FAIL"))
    return ok, lines


def _mk_report(rate, p99, passed=True, basis="relay_e2e_wall",
               alpn="moqt-16", notes=None, impl="moq5-lanes2"):
    return json.dumps({
        "impl": impl,
        "correctness": {"passed": passed},
        "ergonomics_notes": notes if notes is not None else
            ["retrieval=subscribe (live ABSOLUTE_START fan-out)"],
        "fairness": {
            "alpn": alpn, "namespace_count": 1, "tracks_per_namespace": 1,
            "publishers_per_namespace": 1, "subscribers_per_track": 16,
            "groups": 16, "objects_per_group": 64,
            "first_object_size": 512, "other_object_size": 512,
        },
        "metrics": {
            "measurement_basis": basis,
            "wall_objects_per_sec": rate,
            "latency_p99_ns": p99,
        },
    })


def selftest():
    fails = 0
    # (a) identical distributions pass
    blocks = []
    for i in range(7):
        m, _ = load_metrics("<a>", _mk_report(100000 + 100 * i, 1.5e8))
        blocks.append((m, dict(m)))
    ok, _ = decide(blocks, 7, 0.02)
    if ok:
        print("PASS: identical blocks pass the gate")
    else:
        sys.stderr.write("FAIL: identical blocks rejected\n")
        fails += 1
    # (b) a 10% rate regression fails
    blocks = []
    for i in range(7):
        old, _ = load_metrics("<b>", _mk_report(100000 + 100 * i, 1.5e8))
        new, _ = load_metrics("<b>", _mk_report(90000 + 100 * i, 1.5e8))
        blocks.append((old, new))
    ok, _ = decide(blocks, 7, 0.02)
    if not ok:
        print("PASS: a 10% rate regression fails the gate")
    else:
        sys.stderr.write("FAIL: rate regression admitted\n")
        fails += 1
    # (c) a 10% p99 regression fails
    blocks = []
    for i in range(7):
        old, _ = load_metrics("<c>", _mk_report(100000, 1.5e8 + i * 1e5))
        new, _ = load_metrics("<c>", _mk_report(100000, 1.65e8 + i * 1e5))
        blocks.append((old, new))
    ok, _ = decide(blocks, 7, 0.02)
    if not ok:
        print("PASS: a 10% p99 regression fails the gate")
    else:
        sys.stderr.write("FAIL: p99 regression admitted\n")
        fails += 1
    # (d) incomplete blocks fail closed
    ok, lines = decide(blocks[:6], 7, 0.02)
    if not ok and "fail closed" in lines[0]:
        print("PASS: incomplete blocks fail the gate closed")
    else:
        sys.stderr.write("FAIL: incomplete blocks admitted\n")
        fails += 1
    # (e) a correctness-failed report never feeds the CI
    m, why = load_metrics("<e>", _mk_report(100000, 1.5e8, passed=False))
    if m is None and "correctness" in why:
        print("PASS: correctness-failed reports are refused")
    else:
        sys.stderr.write("FAIL: failed report admitted\n")
        fails += 1
    # (f) non-finite / boolean metrics are refused
    m1, w1 = load_metrics("<f>", _mk_report(float("inf"), 1.5e8))
    m2, w2 = load_metrics("<f>",
                          '{"correctness":{"passed":true},'
                          '"ergonomics_notes":["retrieval=subscribe x"],'
                          '"fairness":{"alpn":"moqt-16","namespace_count":1,'
                          '"tracks_per_namespace":1,'
                          '"publishers_per_namespace":1,'
                          '"subscribers_per_track":16,"groups":16,'
                          '"objects_per_group":64,"first_object_size":512,'
                          '"other_object_size":512},'
                          '"metrics":{"measurement_basis":"relay_e2e_wall",'
                          '"wall_objects_per_sec":true,'
                          '"latency_p99_ns":150000000}}')
    if m1 is None and m2 is None:
        print("PASS: non-finite and boolean metrics are refused")
    else:
        sys.stderr.write("FAIL: bad metrics admitted %r %r\n" % (w1, w2))
        fails += 1
    # (g) the wrong measurement class is refused
    m, why = load_metrics("<g>", _mk_report(1e5, 1.5e8,
                                            basis="in_process_wall"))
    if m is None and "relay_e2e_wall" in why:
        print("PASS: non-relay_e2e_wall reports are refused")
    else:
        sys.stderr.write("FAIL: wrong basis admitted\n")
        fails += 1
    # (h) mismatched fingerprints across old/new fail the whole decision
    blocks = []
    for i in range(7):
        old, _ = load_metrics("<h>", _mk_report(1e5, 1.5e8))
        new, _ = load_metrics("<h>", _mk_report(1e5, 1.5e8,
                                                alpn="moqt-18"))
        blocks.append((old, new))
    ok, lines = decide(blocks, 7, 0.02)
    if not ok and "fingerprints" in lines[0]:
        print("PASS: mismatched workload fingerprints fail the decision")
    else:
        sys.stderr.write("FAIL: mixed fingerprints admitted\n")
        fails += 1
    # (f2) fingerprint VALUES are validated: negative/boolean counts,
    # non-string alpn, unknown retrieval mode all refused
    bad_fps = [
        _mk_report(1e5, 1.5e8).replace('"subscribers_per_track": 16',
                                       '"subscribers_per_track": -16'),
        _mk_report(1e5, 1.5e8).replace('"groups": 16', '"groups": true'),
        _mk_report(1e5, 1.5e8).replace('"alpn": "moqt-16"', '"alpn": 7'),
        _mk_report(1e5, 1.5e8, notes=["retrieval=garbage x"]),
        _mk_report(1e5, 1.5e8).replace('"namespace_count": 1',
                                       '"namespace_count": 0'),
    ]
    if all(load_metrics("<f2>", t)[0] is None for t in bad_fps):
        print("PASS: fingerprint values validated (sign/bool/type/mode/"
              "zero-deliveries)")
    else:
        sys.stderr.write("FAIL: invalid fingerprint values admitted\n")
        fails += 1
    # (h2) a drifted workload on BOTH binaries (identical fingerprints,
    # but not the gate's fixed operating point) fails the decision — every
    # fingerprint dimension is pinned, including payload sizes and the
    # namespace/track/publisher topology
    drifts = [
        ('"subscribers_per_track": 16', '"subscribers_per_track": 8'),
        ('"first_object_size": 512', '"first_object_size": 1024'),
        ('"other_object_size": 512', '"other_object_size": 1024'),
        ('"tracks_per_namespace": 1', '"tracks_per_namespace": 2'),
        ('"publishers_per_namespace": 1', '"publishers_per_namespace": 2'),
    ]
    bad = []
    for old_frag, new_frag in drifts:
        blocks = []
        for i in range(7):
            m, _ = load_metrics("<h2>",
                                _mk_report(1e5, 1.5e8).replace(old_frag,
                                                               new_frag))
            blocks.append((m, dict(m)))
        ok, lines = decide(blocks, 7, 0.02)
        if ok or "fixed operating point" not in lines[0]:
            bad.append(new_frag)
    if not bad:
        print("PASS: every drifted fixed-workload dimension fails the "
              "decision (subs/payloads/topology)")
    else:
        sys.stderr.write("FAIL: drift admitted %r\n" % bad)
        fails += 1
    # (i) tolerance/block-count argument bounds
    import subprocess
    bad_args = [["--blocks", "7", "--tolerance", "inf"],
                ["--blocks", "7", "--tolerance", "0"],
                ["--blocks", "7", "--tolerance", "1"],
                ["--blocks", "0"],
                ["--blocks", "x"],
                ["--no-such-flag"]]
    rcs = [subprocess.run([sys.executable, __file__] + a,
                          capture_output=True).returncode for a in bad_args]
    if all(rc == 2 for rc in rcs):
        print("PASS: tolerance/blocks/flag argument bounds enforced")
    else:
        sys.stderr.write("FAIL: argument bounds %r\n" % rcs)
        fails += 1
    # (m) canonical-integer / fraction rejection for the new flags:
    # underscores, zero, negatives, junk, leading zeros, exponents
    bad_new = [["--expect-subs", "1_6"], ["--expect-subs", "0"],
               ["--expect-subs", "-3"], ["--expect-subs", "24x"],
               ["--expect-subs", "016"], ["--expect-lanes", "1_0"],
               ["--expect-groups", "0"], ["--expect-objects", "6 4"],
               ["--expect-payload", "+512"], ["--min-gain", "1_0"],
               ["--min-gain", "10"], ["--min-gain", "-0.1"],
               ["--min-gain", "1e-1"], ["--min-gain", "0"]]
    rcs = [subprocess.run([sys.executable, __file__, "--blocks", "7"] + a,
                          capture_output=True).returncode for a in bad_new]
    if all(rc == 2 for rc in rcs):
        print("PASS: canonical parsing enforced on the new flags")
    else:
        sys.stderr.write("FAIL: new-flag parsing %r\n" % rcs)
        fails += 1
    # (n) improvement gate: positive median whose CI crosses zero FAILS
    blocks = []
    deltas = [+0.12, +0.08, -0.06, +0.02, -0.04, +0.10, -0.01]
    for i, dl in enumerate(deltas):
        old, _ = load_metrics("<n>", _mk_report(1e5, 1.5e8))
        new_, _ = load_metrics("<n>", _mk_report(1e5 * (1 + dl), 1.5e8))
        blocks.append((old, new_))
    ok, lines = decide(blocks, 7, 0.02, None, require_improvement=True)
    if not ok and any("NOT-SIGNIFICANT" in ln for ln in lines):
        print("PASS: positive-median/zero-crossing CI fails improvement")
    else:
        sys.stderr.write("FAIL: insignificant improvement admitted: %r\n"
                         % lines)
        fails += 1
    # (o) significant but below the minimum gain FAILS; above it PASSES
    blocks = []
    for i in range(7):
        old, _ = load_metrics("<o>", _mk_report(1e5, 1.5e8))
        new_, _ = load_metrics("<o>", _mk_report(1e5 * 1.06, 1.5e8))
        blocks.append((old, new_))
    ok_low, lines_low = decide(blocks, 7, 0.02, None,
                               require_improvement=True, min_gain=0.10)
    ok_hi, _ = decide(blocks, 7, 0.02, None,
                      require_improvement=True, min_gain=0.05)
    if (not ok_low and any("BELOW-MINIMUM" in ln for ln in lines_low)
            and ok_hi):
        print("PASS: minimum-gain median threshold enforced")
    else:
        sys.stderr.write("FAIL: min-gain gate %r %r\n" % (ok_low, ok_hi))
        fails += 1
    # (p) lane identity: an impl that is not the expected lane count FAILS
    blocks = []
    for i in range(7):
        old, _ = load_metrics("<p>", _mk_report(1e5, 1.5e8,
                                                impl="moq5-lanes1"))
        new_, _ = load_metrics("<p>", _mk_report(1e5, 1.5e8,
                                                 impl="moq5-lanes1"))
        blocks.append((old, new_))
    fp8 = dict(EXPECT_FP)
    fp8["impl"] = "moq5-lanes8"
    ok, lines = decide(blocks, 7, 0.02, fp8)
    if not ok and "impl" in lines[0]:
        print("PASS: lane identity (impl) is fingerprint-enforced")
    else:
        sys.stderr.write("FAIL: lane mismatch admitted %r\n" % lines)
        fails += 1
    # (q2) --min-gain without --require-improvement is refused
    rc = subprocess.run([sys.executable, __file__, "--blocks", "7",
                         "--min-gain", "0.05"],
                        capture_output=True).returncode
    if rc == 2:
        print("PASS: --min-gain demands --require-improvement")
    else:
        sys.stderr.write("FAIL: min-gain without significance accepted\n")
        fails += 1
    # (q) mixed impl across arms is a mixed fingerprint
    old, _ = load_metrics("<q>", _mk_report(1e5, 1.5e8, impl="moq5-lanes1"))
    new_, _ = load_metrics("<q>", _mk_report(1e5, 1.5e8, impl="moq5-lanes8"))
    ok, lines = decide([(old, new_)] * 7, 7, 0.02)
    if not ok and "fingerprints" in lines[0]:
        print("PASS: mixed lane impls never meet in one CI")
    else:
        sys.stderr.write("FAIL: mixed impls admitted %r\n" % lines)
        fails += 1
    if fails == 0:
        print("# ALL PASS")
    return 1 if fails else 0


def parse_canon_pos_int(flag, text):
    """Canonical positive decimal integer: digits only, no leading zero,
    no sign, no underscores (Python's int() would accept '1_6')."""
    import re
    if not re.fullmatch(r"[1-9][0-9]*", text):
        die("%s must be a canonical positive integer, got %r" % (flag, text))
    return int(text)


def parse_canon_frac(flag, text):
    """Canonical decimal fraction 0 < f < 1 (e.g. 0.10 or .10) — no
    exponents, signs, underscores, nan/inf."""
    import re
    if not re.fullmatch(r"(0?\.[0-9]+)", text):
        die("%s must be a plain decimal with 0 < f < 1, got %r" % (flag, text))
    v = float(text)
    if not (0.0 < v < 1.0):
        die("%s must satisfy 0 < f < 1, got %r" % (flag, text))
    return v


def main(argv):
    pairs = []
    expect = None
    tolerance = 0.02
    expect_subs = None
    expect_lanes = None
    expect_groups = None
    expect_objects = None
    expect_payload = None
    require_improvement = False
    min_gain = None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--selftest":
            return selftest()
        elif a == "--blocks":
            i += 1
            if i >= len(argv):
                die("--blocks needs a value")
            try:
                expect = int(argv[i])
            except ValueError:
                die("--blocks not an integer: %s" % argv[i])
            if expect < 1:
                die("--blocks must be a positive integer")
        elif a == "--tolerance":
            i += 1
            if i >= len(argv):
                die("--tolerance needs a value")
            try:
                tolerance = float(argv[i])
            except ValueError:
                die("--tolerance not a number: %s" % argv[i])
            if not (math.isfinite(tolerance) and 0.0 < tolerance < 1.0):
                die("--tolerance must be finite with 0 < t < 1")
        elif a == "--expect-subs":
            i += 1
            if i >= len(argv):
                die("--expect-subs needs a value")
            expect_subs = parse_canon_pos_int(a, argv[i])
        elif a == "--expect-lanes":
            i += 1
            if i >= len(argv):
                die("--expect-lanes needs a value")
            expect_lanes = parse_canon_pos_int(a, argv[i])
        elif a == "--expect-groups":
            i += 1
            if i >= len(argv):
                die("--expect-groups needs a value")
            expect_groups = parse_canon_pos_int(a, argv[i])
        elif a == "--expect-objects":
            i += 1
            if i >= len(argv):
                die("--expect-objects needs a value")
            expect_objects = parse_canon_pos_int(a, argv[i])
        elif a == "--expect-payload":
            i += 1
            if i >= len(argv):
                die("--expect-payload needs a value")
            expect_payload = parse_canon_pos_int(a, argv[i])
        elif a == "--require-improvement":
            require_improvement = True
        elif a == "--min-gain":
            i += 1
            if i >= len(argv):
                die("--min-gain needs a value")
            min_gain = parse_canon_frac(a, argv[i])
        elif a == "--block":
            if i + 2 >= len(argv):
                die("--block needs <old.json> <new.json>")
            pairs.append((argv[i + 1], argv[i + 2]))
            i += 2
        else:
            die("unknown flag: %s" % a)
        i += 1
    if expect is None:
        die("--blocks is required")
    if min_gain is not None and not require_improvement:
        die("--min-gain is an ADDITIONAL threshold on a significant "
            "improvement: it requires --require-improvement")
    blocks = []
    for (op, np_) in pairs:
        om, why = load_metrics(op)
        if om is None:
            die(why)
        nm, why = load_metrics(np_)
        if nm is None:
            die(why)
        blocks.append((om, nm))
    expect_fp = dict(EXPECT_FP)
    if expect_subs is not None:
        expect_fp["subscribers_per_track"] = expect_subs
    if expect_lanes is not None:
        expect_fp["impl"] = "moq5-lanes%d" % expect_lanes
    if expect_groups is not None:
        expect_fp["groups"] = expect_groups
    if expect_objects is not None:
        expect_fp["objects_per_group"] = expect_objects
    if expect_payload is not None:
        expect_fp["first_object_size"] = expect_payload
        expect_fp["other_object_size"] = expect_payload
    ok, lines = decide(blocks, expect, tolerance, expect_fp,
                       require_improvement, min_gain)
    for ln in lines:
        print(ln)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
