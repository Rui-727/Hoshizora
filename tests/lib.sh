#!/bin/bash
# tests/lib.sh — shared helpers for self-checks. Source from sibling test scripts.
# deferred: one helper, three callers. Add more when 4+ scripts need them.

PASS=0
FAIL=0

chk() {
    if grep -q "$1" "$LOGFILE" 2>/dev/null; then
        echo "PASS: $2"
        PASS=$((PASS+1))
    else
        echo "FAIL: $2"
        FAIL=$((FAIL+1))
    fi
}

summary() {
    local name="$1"
    echo "--- $name self-check: $PASS PASS, $FAIL FAIL ---"
    [ "$FAIL" -eq 0 ] || exit 1
}
