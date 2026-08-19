#!/usr/bin/env bash
# Manual closed-loop CEILING sweep for the owner-boundary saturation curve.
# NOT run by CI. Sweeps K (and, per --regime, F / oversubscription) at a fixed
# workload, calibrating CAP per point until B* plateaus, and emits raw CSV rows
# (one per rep) plus a median/min/max + knee summary via shard_saturation_agg.py.
#
# The canonical curve requires every lane pinned to a distinct CPU (Linux
# cpuset). On a host without strict affinity the rows carry pinned=0 and are
# EXPLORATORY only — never a published baseline.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
bench="${BENCH:-}"
agg="$here/shard_saturation_agg.py"
out="${OUT:-/tmp/shard_ceiling_raw.csv}"
shape="whole"
size="257"
reps="7"
ks="2 4 8 16"
caps="32 64 128 256 512"
regime="primary"   # primary | fanoutF | oversub
fanout="1"

usage() {
    cat >&2 <<EOF
usage: BENCH=/path/to/bench_relay_saturation $0 [options]
  --shape whole|chunked     object shape (default whole)
  --size N                  payload/chunk bytes (default 257)
  --reps N                  reps per calibrated point (default 7, >=7 for canonical)
  --ks "2 4 8 ..."          shard counts to sweep (default "2 4 8 16")
  --caps "32 64 ..."        CAP ladder for plateau calibration (default "32..512")
  --fanout N                local fanout F (default 1; primary curve is F=1)
  --regime primary|fanoutF|oversub   labeled regime (default primary)
  --out PATH                raw CSV path (default $out)
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
        --caps) caps="$2"; shift 2;;
        --fanout) fanout="$2"; shift 2;;
        --regime) regime="$2"; shift 2;;
        --out) out="$2"; shift 2;;
        -h|--help) usage;;
        *) echo "unknown argument: $1" >&2; usage;;
    esac
done

[ -n "$bench" ] || { echo "BENCH not set" >&2; usage; }
[ -x "$bench" ] || { echo "BENCH not executable: $bench" >&2; exit 2; }

echo "# ceiling sweep regime=$regime shape=$shape size=$size fanout=$fanout reps=$reps" >&2
echo "# ks=[$ks] caps=[$caps] -> $out" >&2

# Header once (the bench prints its own header on the first row).
: > "$out"
first=1
for k in $ks; do
    # CAP calibration: collect the full CAP ladder (reps at every rung); the
    # AGGREGATOR performs the calibration — rows are grouped per (workload, CAP)
    # so different CAPs never pool into one median, each rung needs min-reps
    # valid reps, and the first explicit plateau rung (next rung raises median
    # B* by <= 5%) is selected. Starved rows never contribute a ceiling.
    for cap in $caps; do
        for rep in $(seq 1 "$reps"); do
            if [ "$first" = "1" ]; then
                "$bench" --campaign ceiling --shards "$k" --fanout "$fanout" \
                    --shape "$shape" --payload-bytes "$size" --cap "$cap" \
                    --objects 512 --format csv >> "$out"
                first=0
            else
                "$bench" --campaign ceiling --shards "$k" --fanout "$fanout" \
                    --shape "$shape" --payload-bytes "$size" --cap "$cap" \
                    --objects 512 --format csv | tail -n +2 >> "$out"
            fi
        done
    done
done

echo "# raw rows -> $out" >&2
echo "# aggregate (per-CAP grouping + plateau calibration; min-reps=$reps):" >&2
python3 "$agg" --allow-unpinned --min-reps "$reps" "$out"
