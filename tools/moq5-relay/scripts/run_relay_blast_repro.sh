#!/usr/bin/env bash
# Manual real-transport fan-out repro: one relay serve process per (lanes, rep),
# driven by an EXTERNALLY SUPPLIED gauntlet relay-blast binary. Emits one
# relay_e2e_wall JSON report per rep plus a median/min/max summary via
# relay_blast_summarize.py. NOT run by CI; nothing here modifies the gauntlet.
#
# The current blast driver hard-codes ALPN moqt-16, so this repro measures
# draft-16 only; the relay listener version must match (versions [16]).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
summ="$here/relay_blast_summarize.py"

lanes_list="1 2 4"
subs=16
groups=16
objects_per_group=64
payload=512
reps=5
port=9443
timeout_s=120
outdir="/tmp/relay_blast_repro"

usage() {
    cat >&2 <<EOF
usage: RELAY_BIN=/path/moq5-relay BLAST_BIN=/path/gauntlet-relay-blast $0 [options]
  --lanes "1 2 4"        lane counts to sweep (default "1 2 4")
  --subs N               subscribers on the single track (default 16)
  --groups N             groups to publish (default 16)
  --objects-per-group N  objects per group (default 64)
  --payload N            object payload bytes (default 512)
  --reps N               repetitions per lane count (default 5)
  --port N               relay UDP port (default 9443)
  --timeout-s N          per-rep blast timeout (default 120)
  --out DIR              output directory (default $outdir)
  --selftest             structural check with stub binaries (no gauntlet)
Environment (required): RELAY_BIN, BLAST_BIN. Optional: CERT, KEY (default:
cert.pem/key.pem beside RELAY_BIN).
EOF
    exit 2
}

selftest_mode=0

# Strict flag parsing FIRST (an unknown flag fails before any environment or
# binary is consulted).
while [ $# -gt 0 ]; do
    case "$1" in
        --lanes) lanes_list="$2"; shift 2;;
        --subs) subs="$2"; shift 2;;
        --groups) groups="$2"; shift 2;;
        --objects-per-group) objects_per_group="$2"; shift 2;;
        --payload) payload="$2"; shift 2;;
        --reps) reps="$2"; shift 2;;
        --port) port="$2"; shift 2;;
        --timeout-s) timeout_s="$2"; shift 2;;
        --out) outdir="$2"; shift 2;;
        --selftest) selftest_mode=1; shift;;
        -h|--help) usage;;
        *) echo "unknown argument: $1" >&2; usage;;
    esac
done

# Structural selftest: run the real flow against stub binaries and prove the
# summary consumes EXACTLY this invocation's manifest — a stale report already
# sitting in the output directory (from an older run with different knobs)
# must not appear in the summary.
if [ "$selftest_mode" = "1" ]; then
    st="$(mktemp -d)"
    trap 'rm -rf "$st"' EXIT
    # The stub relay prints a valid single-lane stats record on SIGTERM (the
    # production relay emits its rows post-stop), so paired-record validation
    # exercises the real path.
    cat > "$st/fake-relay" <<'STUB'
#!/bin/sh
row="RELAY_LANE_STATS_V1,lane=0,wakes_same_lane=0,wakes_cross_lane=0"
row="$row,wakes_external=0,wakes_coalesced=0,pump_sweeps=1,deadline_sweeps=0"
row="$row,idle_cap_wakes=0,wake_to_pump_max_us=0,wake_to_pump_total_us=0"
row="$row,wake_to_pump_samples=0,service_passes=1,flush_sends=1,flush_bytes=8"
row="$row,pump_turns=1,pump_messages=0,pump_bytes=0,wake_requests_push=0"
row="$row,wake_requests_credit=0,enq_demand=0,enq_undemand=0,enq_done=0"
row="$row,enq_ack=0,enq_obj=0,enq_obj_open=0,enq_obj_chunk=0,enq_obj_end=0"
row="$row,enq_obj_reset=0,enq_grp_reset=0,enq_grp_evict=0,enq_sg_seal=0"
row="$row,channel_entries_hwm=0,channel_bytes_hwm=0,eor=1"
trap 'echo "$row"; exit 0' TERM INT
while :; do sleep 1; done
STUB
    # The stub blast writes a canned relay_e2e_wall report matching the
    # scenario the repro generates (16 subs at the selftest's knobs below).
    cat > "$st/fake-blast" <<'STUB'
#!/bin/sh
out=""
while [ $# -gt 0 ]; do
    if [ "$1" = "--out" ]; then out="$2"; shift 2; else shift; fi
done
cat > "$out" <<'JSON'
{"impl":"moq5-lanes1","correctness":{"passed":true},
 "ergonomics_notes":["retrieval=subscribe (live ABSOLUTE_START fan-out)"],
 "fairness":{"alpn":"moqt-16","namespace_count":1,"tracks_per_namespace":1,
  "publishers_per_namespace":1,"subscribers_per_track":2,"groups":2,
  "objects_per_group":2,"first_object_size":64,"other_object_size":64},
 "metrics":{"measurement_basis":"relay_e2e_wall","objects_delivered":8,
  "bytes_delivered":512,"wall_objects_per_sec":1000.0,
  "latency_p50_ns":1000000,"latency_p99_ns":2000000}}
JSON
STUB
    # The failing stub: writes the SAME plausible report but exits 4 (an
    # engine/config failure) — the rep must be rejected despite the file.
    cp "$st/fake-blast" "$st/fake-blast-exit4"
    echo "exit 4" >> "$st/fake-blast-exit4"
    chmod +x "$st/fake-blast-exit4"
    chmod +x "$st/fake-relay" "$st/fake-blast"
    touch "$st/cert.pem" "$st/key.pem"
    mkdir -p "$st/out"
    # The stale report: an OLDER run's file with a DIFFERENT workload (99
    # subscribers) and a poison rate; a glob over the directory would ingest it.
    cat > "$st/out/blast-l9-r9.json" <<'JSON'
{"impl":"moq5-lanes9","correctness":{"passed":true},
 "fairness":{"alpn":"moqt-16","namespace_count":1,"tracks_per_namespace":1,
  "publishers_per_namespace":1,"subscribers_per_track":99,"groups":2,
  "objects_per_group":2,"first_object_size":64,"other_object_size":64},
 "metrics":{"measurement_basis":"relay_e2e_wall","objects_delivered":396,
  "bytes_delivered":512,"wall_objects_per_sec":999999.0,
  "latency_p50_ns":1,"latency_p99_ns":1}}
JSON
    summary="$(RELAY_BIN="$st/fake-relay" BLAST_BIN="$st/fake-blast" \
        CERT="$st/cert.pem" KEY="$st/key.pem" \
        "$0" --lanes 1 --reps 1 --subs 2 --groups 2 --objects-per-group 2 \
             --payload 64 --timeout-s 30 --out "$st/out" 2>/dev/null)"
    fails=0
    case "$summary" in *moq5-lanes1*) ;; *)
        echo "FAIL: fresh report missing from summary" >&2; fails=1;; esac
    case "$summary" in *moq5-lanes9*|*999999*)
        echo "FAIL: stale report leaked into the summary" >&2; fails=1;; esac
    if [ "$fails" = "0" ]; then
        echo "PASS: summary consumes only this invocation's manifest"
    fi
    # A blast exit code above 1 (usage/scenario/engine/config failure) must
    # invalidate the rep even though a plausible report file was written.
    mkdir -p "$st/out2"
    err2="$st/err2.log"
    set +e
    RELAY_BIN="$st/fake-relay" BLAST_BIN="$st/fake-blast-exit4" \
        CERT="$st/cert.pem" KEY="$st/key.pem" \
        "$0" --lanes 1 --reps 1 --subs 2 --groups 2 --objects-per-group 2 \
             --payload 64 --timeout-s 30 --out "$st/out2" \
             > "$st/out2/summary.txt" 2> "$err2"
    rc2=$?
    set -e
    if [ "$rc2" = "1" ] && grep -q "blast exit 4" "$err2" && \
       ! grep -q "moq5-lanes1" "$st/out2/summary.txt"; then
        echo "PASS: blast exit 4 rejects the rep despite a report file"
    else
        echo "FAIL: exit-4 rep not rejected (rc=$rc2)" >&2
        fails=1
    fi
    if [ "$fails" = "0" ]; then
        echo "# ALL PASS"
        exit 0
    fi
    exit 1
fi

# Prerequisite validation: every external piece must exist before any process
# starts (a missing binary is a config error, not a mid-run surprise).
relay_bin="${RELAY_BIN:-}"
blast_bin="${BLAST_BIN:-}"
[ -n "$relay_bin" ] || { echo "RELAY_BIN not set" >&2; exit 2; }
[ -n "$blast_bin" ] || { echo "BLAST_BIN not set" >&2; exit 2; }
[ -x "$relay_bin" ] || { echo "RELAY_BIN not executable: $relay_bin" >&2; exit 2; }
[ -x "$blast_bin" ] || { echo "BLAST_BIN not executable: $blast_bin" >&2; exit 2; }
cert="${CERT:-$(dirname "$relay_bin")/cert.pem}"
key="${KEY:-$(dirname "$relay_bin")/key.pem}"
[ -r "$cert" ] || { echo "cert not readable: $cert" >&2; exit 2; }
[ -r "$key" ] || { echo "key not readable: $key" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 2; }
[ -f "$summ" ] || { echo "summarizer missing: $summ" >&2; exit 2; }

mkdir -p "$outdir"

# The tight single-track fan-out scenario (relay-owned; the gauntlet checkout
# is never modified — this file is INPUT to the blast binary).
scenario="$outdir/single_track_fanout.json"
cat > "$scenario" <<EOF
{
  "name": "relay_lane_fanout_repro",
  "namespace_count": 1,
  "tracks_per_namespace": 1,
  "publishers_per_namespace": 1,
  "subscribers_per_track": $subs,
  "payload": {
    "first_object_size": $payload,
    "other_object_size": $payload,
    "objects_per_group": $objects_per_group,
    "groups": $groups,
    "seed": 7
  },
  "cache": { "mode": "hit", "preloaded_groups": $groups },
  "shard": {
    "shard_count": 1,
    "publisher_placement": "round_robin",
    "subscriber_placement": "colocated_with_publisher",
    "cross_shard_policy": "clone"
  },
  "fairness": { "warmup_s": 0, "measure_s": 0, "alpn": "moqt-16" },
  "assert": { "exact_bytes": true, "no_drops_on_healthy_subs": true }
}
EOF

echo "# repro: subs=$subs objects=$((groups * objects_per_group)) payload=$payload reps=$reps timeout=${timeout_s}s" >&2
echo "# relay=$relay_bin" >&2
echo "# blast=$blast_bin" >&2

timeouts=0
rejected=0
manifest=()   # EXACTLY this invocation's reps as --rep triples
              # (report.json relay.log lanes) — never a glob, and each blast
              # JSON is paired with the relay log from the SAME rep so the
              # summarizer can validate that rep's lane-stat record against
              # its configured lane count
for lanes in $lanes_list; do
    cfg="$outdir/relay-l$lanes.json"
    cat > "$cfg" <<EOF
{
  "listener": {
    "transport": "msquic",
    "host": "127.0.0.1",
    "port": $port,
    "cert": "$cert",
    "key": "$key",
    "versions": [16],
    "lanes": $lanes
  }
}
EOF
    for rep in $(seq 1 "$reps"); do
        report="$outdir/blast-l$lanes-r$rep.json"
        rm -f "$report"
        "$relay_bin" serve --config "$cfg" \
            > "$outdir/relay-l$lanes-r$rep.log" 2>&1 &
        relay_pid=$!
        sleep 1
        if ! kill -0 "$relay_pid" 2>/dev/null; then
            echo "relay failed to start (lanes=$lanes); see $outdir/relay-l$lanes-r$rep.log" >&2
            exit 1
        fi
        # Bounded blast run (macOS has no coreutils timeout(1)): background +
        # poll; on expiry the rep is recorded as a timeout, not a hang.
        "$blast_bin" --scenario "$scenario" --relay "127.0.0.1:$port" \
            --impl "moq5-lanes$lanes" --out "$report" \
            > "$outdir/blast-l$lanes-r$rep.log" 2>&1 &
        blast_pid=$!
        waited=0
        while kill -0 "$blast_pid" 2>/dev/null && [ "$waited" -lt "$timeout_s" ]; do
            sleep 1
            waited=$((waited + 1))
        done
        if kill -0 "$blast_pid" 2>/dev/null; then
            kill -9 "$blast_pid" 2>/dev/null || true
            rm -f "$report"   # a partial report must never enter the summary
            timeouts=$((timeouts + 1))
            echo "TIMEOUT lanes=$lanes rep=$rep after ${timeout_s}s" >&2
            wait "$blast_pid" 2>/dev/null || true
        else
            # The blast exit code is load-bearing: 0 = pass, 1 = correctness
            # failure WITH a valid report; anything higher (usage/scenario/
            # engine/config failure) invalidates the rep even if a report
            # file exists.
            blast_rc=0
            wait "$blast_pid" 2>/dev/null || blast_rc=$?
            if [ "$blast_rc" -le 1 ] && [ -f "$report" ]; then
                manifest+=("--rep" "$report"
                           "$outdir/relay-l$lanes-r$rep.log" "$lanes")
            else
                rm -f "$report"
                rejected=$((rejected + 1))
                echo "REJECTED lanes=$lanes rep=$rep: blast exit $blast_rc" \
                     "(only 0/1 admit a report)" >&2
            fi
        fi
        kill "$relay_pid" 2>/dev/null || true
        wait "$relay_pid" 2>/dev/null || true
    done
done

echo "# timeouts: $timeouts, rejected: $rejected (each rep bounded at" >&2
echo "#   ${timeout_s}s; the blast driver additionally arms ONE internal 15s" >&2
echo "#   deadline before ANNOUNCE and reuses it for setup + delivery, so the" >&2
echo "#   delivery phase always gets less than 15s regardless of --timeout-s)" >&2
echo "# summary (relay_e2e_wall; canonical = passed reps at full replication" >&2
echo "#   only; each rep is a report+relay-log pair from THIS invocation," >&2
echo "#   never a directory glob; ONE invalid lane-stat record — refusal," >&2
echo "#   malformed row, or count != configured lanes — fails the whole" >&2
echo "#   summary, so counter aggregates are all-or-nothing):" >&2
if [ "${#manifest[@]}" -eq 0 ]; then
    echo "no completed reports (every rep timed out or was rejected)" >&2
    exit 1
fi
python3 "$summ" --expect-reps "$reps" "${manifest[@]}"
