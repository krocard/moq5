#!/usr/bin/env bash
# Manual fixed-rate OPEN-LOOP knee sweep. NOT run by CI. For each K it sweeps the
# requested offered rate lambda upward; the duration T at each lambda is sized so
# lambda*T stays under the fixed one-group retention cap (the bench refuses an
# over-retention point before workers start). Emits raw CSV (one row per rep)
# plus the lambda*(K) knee summary (with a deterministic bootstrap CI) via
# shard_saturation_agg.py.
#
# Canonical knees need distinctly pinned lanes on a low-jitter host: the
# injection-lateness gate rejects points where the owner cannot hit its
# deadlines. On an unpinned/noisy host the rows carry pinned=0 and most knee
# points invalidate on lateness — EXPLORATORY only.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
bench="${BENCH:-}"
agg="$here/shard_saturation_agg.py"
out="${OUT:-/tmp/shard_knee_raw.csv}"
shape="whole"
size="257"
reps="7"
ks="2 4 8"
lambdas="1000 2000 4000 8000 16000 32000"
target_inject="2000"   # objects to inject per point -> T = target/lambda
lateness_fraction="0.25"
skew_fraction="0.10"
rate_tolerance="0.15"

usage() {
    cat >&2 <<EOF
usage: BENCH=/path/to/bench_relay_saturation $0 [options]
  --shape whole|chunked         object shape (default whole)
  --size N                      payload/chunk bytes (default 257)
  --reps N                      reps per lambda (default 7)
  --ks "2 4 8 ..."              shard counts (default "2 4 8")
  --lambdas "1000 2000 ..."     requested offered rates, objects/sec
  --target-inject N             objects to inject per point (sets T; default 2000)
  --lateness-fraction X         max injection lateness / period (default 0.25)
  --skew-fraction X             max endpoint skew / window (default 0.10)
  --rate-tolerance X            max |actual-requested|/requested (default 0.15)
  --out PATH                    raw CSV path (default $out)
Environment: BENCH must point at the built bench_relay_saturation binary.
EOF
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --shape) shape="$2"; shift 2;;
        --size) size="$2"; shift 2;;
        --reps) reps="$2"; shift 2;;
        --ks) ks="$2"; shift 2;;
        --lambdas) lambdas="$2"; shift 2;;
        --target-inject) target_inject="$2"; shift 2;;
        --lateness-fraction) lateness_fraction="$2"; shift 2;;
        --skew-fraction) skew_fraction="$2"; shift 2;;
        --rate-tolerance) rate_tolerance="$2"; shift 2;;
        --out) out="$2"; shift 2;;
        -h|--help) usage;;
        *) echo "unknown argument: $1" >&2; usage;;
    esac
done

[ -n "$bench" ] || { echo "BENCH not set" >&2; usage; }
[ -x "$bench" ] || { echo "BENCH not executable: $bench" >&2; exit 2; }

echo "# knee sweep shape=$shape size=$size reps=$reps target_inject=$target_inject" >&2
echo "# ks=[$ks] lambdas=[$lambdas] -> $out" >&2

: > "$out"
first=1
for k in $ks; do
    for lam in $lambdas; do
        # T (ms) = target_inject / lambda * 1000, floor 20 ms.
        dur=$(( target_inject * 1000 / lam ))
        [ "$dur" -lt 20 ] && dur=20
        for rep in $(seq 1 "$reps"); do
            args=(--campaign knee --shards "$k" --fanout 1 --shape "$shape" \
                  --payload-bytes "$size" --lambda "$lam" --duration-ms "$dur" \
                  --rate-tolerance "$rate_tolerance" \
                  --lateness-fraction "$lateness_fraction" \
                  --endpoint-skew-fraction "$skew_fraction" --format csv)
            if [ "$first" = "1" ]; then
                "$bench" "${args[@]}" >> "$out"; first=0
            else
                "$bench" "${args[@]}" | tail -n +2 >> "$out"
            fi
        done
    done
done

echo "# raw rows -> $out" >&2
echo "# aggregate (epsilon=0.10; min-reps=$reps):" >&2
python3 "$agg" --allow-unpinned --min-reps "$reps" "$out"
