#!/bin/bash
# Hoshizora testsuite — single entry point for the whole test surface.
# deferred: one shell script, no test framework. Runs each self-check in
# sequence, captures PASS/FAIL counts, exits non-zero if any FAIL.
#
# Layout (each self-check has its own .hs config + its own .sh, paired):
#   core.sh     + core.hs     — parse, fork+exec, deps, exec-fail, CLI verbs
#   features.sh + features.hs — parser + runtime for cgroup/watch/start-cond/
#                               healthy/backoff-max/oom-kill/log/network_ready
#   onfail.sh   + onfail.hs   — on-fail: directive (parser + runtime firing)
#
# Run: bash tests/testsuite.sh   or   make test
cd "$(dirname "$0")"   # tests/ — self-checks are siblings

PASS=0
FAIL=0
FAILED_SCRIPTS=()

run() {
    local name="$1"
    local script="$2"
    echo "========================================================"
    echo "  TESTSUITE: $name"
    echo "========================================================"
    local out
    if out=$(bash "$script" 2>&1); then
        # count PASS/FAIL lines
        local p f
        p=$(printf '%s\n' "$out" | grep -c '^PASS:' || true)
        f=$(printf '%s\n' "$out" | grep -c '^FAIL:' || true)
        PASS=$((PASS + p))
        FAIL=$((FAIL + f))
        if [ "$f" -gt 0 ]; then
            FAILED_SCRIPTS+=("$name")
        fi
        printf '%s\n' "$out" | tail -40
        echo "  -> $name: $p PASS, $f FAIL"
    else
        FAIL=$((FAIL + 1))
        FAILED_SCRIPTS+=("$name (non-zero exit)")
        printf '%s\n' "$out" | tail -40
        echo "  -> $name: EXIT NON-ZERO"
    fi
    echo
}

run "core"           core.sh
run "features"       features.sh
run "on-fail"        onfail.sh
run "cron"           cron.sh
run "hzlog"          hzlog.sh
run "v2-features"    v2.sh
run "v2.1-features"  v21.sh
run "plugin"         plugin.sh
run "edge-cases"     edge_cases.sh

echo "========================================================"
echo "  TESTSUITE SUMMARY"
echo "========================================================"
echo "  total PASS: $PASS"
echo "  total FAIL: $FAIL"
if [ "$FAIL" -gt 0 ]; then
    echo "  failed scripts: ${FAILED_SCRIPTS[*]}"
    exit 1
fi
echo "  all checks passed."
