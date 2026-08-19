#!/bin/bash
# Both production serve compositions must route their transport versions through
# the one shared mapper, and neither may write a version field itself. This is a
# SOURCE check because the two call sites live in one binary: an endpoint test
# cannot tell which of them produced a listener.
set -u
main_c="$1"
cmake_txt="$2"
fail=0

note() { echo "  $*"; }

# 1. Exactly the two intended construction sites call the shared mapper, and
#    each is inside a distinct serve composition.
calls=$(grep -c 'moqr_cli_apply_versions(cfg, &tcfg);' "$main_c")
[ "$calls" -eq 2 ] || { echo "FAIL: expected 2 mapper call sites, found $calls"; fail=1; }

# The single-lane site precedes the multi-lane one, which is identified by the
# lane_count assignment that follows it.
single=$(grep -n 'moqr_cli_apply_versions(cfg, &tcfg);' "$main_c" | head -1 | cut -d: -f1)
multi=$(grep -n 'moqr_cli_apply_versions(cfg, &tcfg);' "$main_c" | tail -1 | cut -d: -f1)
[ -n "$single" ] && [ -n "$multi" ] && [ "$single" -lt "$multi" ] || {
    echo "FAIL: could not identify two distinct serve compositions"; fail=1; }
sed -n "$((multi)),$((multi+2))p" "$main_c" | grep -q 'tcfg.lane_count = cfg->lanes;' || {
    echo "FAIL: the second call site is not the multi-lane composition"; fail=1; }

# 2. Neither site writes a version/list field directly afterward.
if grep -nE 'tcfg\.(version|versions|version_count)[[:space:]]*=' "$main_c"; then
    echo "FAIL: a serve path assigns a version field directly"; fail=1
fi

# 3. Both relevant CLI targets link the shared mapper.
for tgt in moq5-relay moq-relay-verify; do
    grep -A3 "add_executable($tgt " "$cmake_txt" | grep -q 'cli/versions.c' || {
        echo "FAIL: target $tgt does not link cli/versions.c"; fail=1; }
done

if [ "$fail" -eq 0 ]; then
    note "single-lane call site: line $single"
    note "multi-lane  call site: line $multi"
    echo "PASS: both serve compositions route through the shared version mapper"
fi
exit $fail
