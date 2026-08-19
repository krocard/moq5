#!/bin/bash
# Both production lane pumps must retire connections through the one shared
# helper, exactly once, AFTER their bind/shard step. This is a SOURCE check
# because the two callbacks live inside the relay binary: no linked test drives
# them, so ordering there can only be proven structurally. Behaviour is proven
# separately, through the same helper, by the real managed-client arms.
#
# The check reads executable code only: comments and string literals are erased
# before any token is counted, and each callback body is delimited by brace
# matching rather than by "up to the next function", so a token in a comment, a
# string, or a neighbouring function can neither satisfy nor distort it.
set -u
main_c="$1"
cmake_txt="$2"
fail=0

note() { echo "  $*"; }

# Erase comments and string/char literals, preserving line numbering so the
# reported positions still refer to the real file.
strip_c() {
    awk '
    BEGIN { inblk = 0 }
    {
        line = $0; out = ""; i = 1; n = length(line)
        while (i <= n) {
            c = substr(line, i, 1); d = substr(line, i, 2)
            if (inblk) {
                if (d == "*/") { inblk = 0; i += 2 } else { i++ }
                continue
            }
            if (d == "/*") { inblk = 1; i += 2; continue }
            if (d == "//") { break }
            if (c == "\"" || c == "'"'"'") {
                q = c; i++
                while (i <= n) {
                    e = substr(line, i, 1)
                    if (e == "\\") { i += 2; continue }
                    if (e == q) { i++; break }
                    i++
                }
                out = out " "
                continue
            }
            out = out c; i++
        }
        print out
    }' "$1"
}

code=$(mktemp); strip_c "$main_c" > "$code"
trap 'rm -f "$code"' EXIT

# Locate each callback by its definition line in the STRIPPED text, then take
# the body by brace matching from its opening brace.
body_of() {
    local fn="$1"
    local start
    start=$(grep -n "^${fn}(moq_msquic_managed_t" "$code" | head -1 | cut -d: -f1)
    [ -n "$start" ] || return 1
    awk -v start="$start" '
    NR < start { next }
    {
        line = $0
        if (!open) {
            p = index(line, "{")
            if (p == 0) { next }
            open = 1; depth = 0
            line = substr(line, p)
        }
        n = length(line)
        for (i = 1; i <= n; i++) {
            c = substr(line, i, 1)
            if (c == "{") depth++
            else if (c == "}") {
                depth--
                if (depth == 0) { print; exit }
            }
        }
        print
    }' "$code"
}

check_pump() {
    local name="$1" step_pattern="$2"
    local body calls step_line reap_line
    body=$(body_of "$name") || { echo "FAIL: could not locate $name"; fail=1; return; }

    calls=$(printf '%s\n' "$body" | grep -c 'moqr_relay_reap_pass[[:space:]]*(')
    [ "$calls" -eq 1 ] || {
        echo "FAIL: $name has $calls executable helper calls, expected exactly 1"
        fail=1; return; }

    step_line=$(printf '%s\n' "$body" | grep -n "$step_pattern" | head -1 | cut -d: -f1)
    reap_line=$(printf '%s\n' "$body" | grep -n 'moqr_relay_reap_pass[[:space:]]*(' \
                | head -1 | cut -d: -f1)
    [ -n "$step_line" ] || { echo "FAIL: $name has no bind/shard step"; fail=1; return; }
    [ "$reap_line" -gt "$step_line" ] || {
        echo "FAIL: $name retires before its step (step@$step_line reap@$reap_line)"
        fail=1; return; }
    note "$name: step at +$step_line, single retirement at +$reap_line"
}

check_pump "relay_lane_pump"  'moqr_bind_pump[[:space:]]*('
check_pump "relay_lanes_pump" 'moqr_shards_step_shard[[:space:]]*('

# Every binary carrying a production pump links the shared helper. Read the
# target's own source list — brace-free but comment-stripped, and bounded to the
# add_executable() call so a mention in another target cannot satisfy it.
cmake_code=$(mktemp); sed 's/#.*//' "$cmake_txt" > "$cmake_code"
trap 'rm -f "$code" "$cmake_code"' EXIT
for tgt in moq5-relay moq-relay-verify moq-relay-measure; do
    awk -v t="add_executable($tgt " '
        index($0, t) { grab = 1 }
        grab { buf = buf " " $0; if (index($0, ")")) { print buf; exit } }
    ' "$cmake_code" | grep -q 'cli/conn_reap\.c' || {
        echo "FAIL: target $tgt does not link cli/conn_reap.c"; fail=1; }
done

[ "$fail" -eq 0 ] && echo "PASS: production reap wiring"
exit "$fail"
