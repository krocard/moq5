#!/usr/bin/env bash
# The moq5-relay command-line surface, driven against the real executable.
#
# Every case here is informational or malformed, so none of them may touch the
# transport, load a configuration, or allocate a runtime: what is being pinned
# is that help and version win before any of that, and that a malformed
# invocation is refused rather than partly honoured.
#
# Argument 1 is the moq5-relay binary.
set -u

bin="${1:-}"
main_c="${2:-}"
if [ ! -x  "$bin" ] || [ ! -f "$main_c" ]; then
    echo "FAIL: usage: $0 <moq5-relay> <cli/main.c>" >&2
    exit 2
fi

failures=0
fail() { echo "FAIL: $1" >&2; failures=$((failures + 1)); }

out=$(mktemp); err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT

# run <expected-exit> <where:stdout|stderr> -- args...
run() {
    local want_rc="$1" where="$2"; shift 3
    : > "$out"; : > "$err"
     "$bin" "$@" > "$out" 2> "$err"
    local rc=$?
    local label="moq5-relay $*"

    if [ "$rc" -ne "$want_rc" ]; then
        fail "$label: exit $rc, expected $want_rc"
    fi
    if [ "$where" = "stdout" ]; then
        [ -s "$out" ] || fail "$label: nothing on stdout"
        [ -s "$err" ] && fail "$label: wrote to stderr"
    else
        [ -s "$err" ] || fail "$label: nothing on stderr"
        [ -s "$out" ] && fail "$label: wrote to stdout"
    fi
}

has() { grep -qF -- "$1" "$2" || fail "$3: missing '$1'"; }

# -- 1. every help and version spelling: stdout, exit 0 --------------------
for a in "-h" "--help" "help"; do
    run 0 stdout -- $a
    has "MOQ5 Relay" "$out" "moq5-relay $a"
    has "Usage:" "$out" "moq5-relay $a"
    has "serve" "$out" "moq5-relay $a"
    has "capacity" "$out" "moq5-relay $a"
done
for a in "-V" "--version"; do
    run 0 stdout -- $a
    has "MOQ5 Relay" "$out" "moq5-relay $a"
    [ "$(wc -l < "$out" | tr -d ' ')" = "1" ] || fail "moq5-relay $a: not one line"
done

# -- 2. scoped help names the option and an example, global help does not --
for spelling in "help serve" "serve -h" "serve --help"; do
    run 0 stdout -- $spelling
    has "--config" "$out" "moq5-relay $spelling"
    has "moq5-relay serve" "$out" "moq5-relay $spelling"
done
for spelling in "help capacity" "capacity -h" "capacity --help"; do
    run 0 stdout -- $spelling
    has "--config" "$out" "moq5-relay $spelling"
    has "moq5-relay capacity" "$out" "moq5-relay $spelling"
done

# -- 3. help wins over a configuration that does not exist -----------------
run 0 stdout -- serve --config /nonexistent/relay.json --help
grep -qi -- "config error" "$err" && fail "help still loaded the config"

# -- 4. help and version win over an invalid verify environment ------------
( export MOQR_VERIFY_BIND_MAX_OPEN_SUBGROUPS=not-a-number
   "$bin" --help >/dev/null 2>&1 ) || fail "--help refused by the verify env"
( export MOQR_VERIFY_BIND_MAX_OPEN_SUBGROUPS=not-a-number
   "$bin" --version >/dev/null 2>&1 ) || fail "--version refused by the verify env"

# -- 5. malformed invocations: stderr, exit 2, and the conventional hint ---
malformed=(
    ""                       # no command
    "bogus"                  # unknown command
    "serve"                  # missing config
    "capacity"               # missing config
    "serve --config"         # missing option value
    "serve -c"               # missing option value
    "serve --nope x"         # unknown option
    "serve --config a --nope"       # unknown option, otherwise well-formed
    "serve --config a --config b"   # duplicate
    "serve --config a extra"        # unexpected positional
    "help bogus"             # unknown help topic
    "--nope"                 # unknown global option
)
for m in "${malformed[@]}"; do
    if [ -z "$m" ]; then
        run 2 stderr --
    else
        run 2 stderr -- $m
    fi
    has "Try 'moq5-relay --help' for more information." "$err" "moq5-relay $m"
done

# -- 6. structured rows are the machine contract, not the prose -----------
# The human prefix carries the product name; the row tokens a harness parses
# must stay byte-identical across any rebranding.
for tok in RELAY_BLOCKED_V0; do
    grep -qF -- "$tok" "$main_c" || fail "structured token '$tok' is gone"
done
# ...and the rebranding did not leak into them.
grep -qE '"MOQ5[_A-Z]*_V[0-9]' "$main_c" && fail "a structured token was rebranded"

if [ "$failures" -ne 0 ]; then
    echo "FAIL: $failures CLI-surface violation(s)" >&2
    exit 1
fi
echo "PASS: moq5-relay CLI surface"
exit 0
