#!/usr/bin/env bash
#
# Relay boundary guard.
#
# tools/moq5-relay/ consumes libmoq strictly through the PUBLIC session API
# and the managed adapter facades. This check fails the build if relay code
# reaches below that line:
#
#   1. Nothing under tools/moq5-relay/ may include private or wire-tooling
#      headers (transport bridge, wire codecs, session internals) or the
#      publisher/subscriber facades (they own session event polling; the
#      relay needs the raw event stream).
#   2. The sans-I/O relay core (tools/moq5-relay/core/) additionally must not
#      include adapter, service-tier, threading, or socket headers — time and
#      I/O are inputs there, same discipline as libmoq core/.
#
# Exit 0 if clean, 1 if violations found.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RELAY_DIR="tools/moq5-relay"

if [ ! -d "$ROOT/$RELAY_DIR" ]; then
    echo "relay boundary: $RELAY_DIR not present; nothing to check"
    exit 0
fi

# Headers no relay code may include (private SPI, wire tooling, internals,
# and the event-owning facades).
FORBIDDEN_EVERYWHERE=(
    'moq/transport_bridge\.h'
    'moq/wire\.h'
    'moq/codec\.h'
    'moq/control\.h'
    'moq/control_d18\.h'
    'moq/kvp\.h'
    'moq/vi64\.h'
    'moq/buf\.h'
    'moq/publisher\.h'
    'moq/subscriber\.h'
    'session_internal\.h'
)

# Additionally forbidden in the sans-I/O relay core.
FORBIDDEN_IN_CORE=(
    'moq/endpoint\.h'
    'moq/media_receiver\.h'
    'moq/media_sender\.h'
    'moq/picoquic'
    'moq/mvfst'
    'moq/pico_wt'
    'moq/proxygen'
    'pthread\.h'
    'threads\.h'
    'sys/socket\.h'
    'netinet/'
)

failures=0

scan() {
    local scope_dir="$1"
    local label="$2"
    shift 2
    local patterns=("$@")

    local files
    files="$(cd "$ROOT" && find "$scope_dir" -type f \
        \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
        -print | sort)"
    [ -n "$files" ] || return 0

    local pat
    for pat in "${patterns[@]}"; do
        local hits
        hits="$(cd "$ROOT" && echo "$files" | \
            xargs grep -nE "^[[:space:]]*#[[:space:]]*include[[:space:]]*[<\"].*${pat}" \
            2>/dev/null || true)"
        if [ -n "$hits" ]; then
            echo "relay boundary violation ($label): forbidden include '${pat}':"
            echo "$hits"
            failures=1
        fi
    done
}

scan "$RELAY_DIR"       "all relay code"    "${FORBIDDEN_EVERYWHERE[@]}"
scan "$RELAY_DIR/core"  "sans-I/O core"     "${FORBIDDEN_IN_CORE[@]}"

if [ "$failures" -ne 0 ]; then
    echo "relay boundary: FAIL"
    exit 1
fi

echo "relay boundary: clean"
exit 0
