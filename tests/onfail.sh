#!/bin/bash
# Hoshizora on-fail runtime self-check.
# deferred: one config, one assertion path — service crashes past max_restarts,
# on_fail_trigger fires `shutdown`, hoshizora exits cleanly without SIGTERM.
#
# Flow:
#   1. flaky service starts (/bin/false exits 1)
#   2. reap_children: state → STOPPED, respawn_at = mono_now() + 1
#   3. Respawn timer (CLOCK_MONOTONIC) fires 1s later
#   4. start_service: restart_count(1) >= max_restarts(1) → mark_failed
#   5. on_fail_trigger: "flaky: on-fail: shutdown" + g_shutdown = 1
#   6. Main loop exits, "shutdown signal received"
#   7. reboot(RB_POWER_OFF) fails (no CAP_SYS_BOOT in sandbox) → log + halt
cd "$(dirname "$0")/.."   # repo root, where ./hoshizora lives
. tests/lib.sh

HZ=./hoshizora
SOCKDIR=/tmp/hz_onfail_$$
SOCK=$SOCKDIR/control
LOGFILE=/tmp/hz_onfail.log
mkdir -p "$SOCKDIR"
trap 'rm -rf "$SOCKDIR"; kill $PID 2>/dev/null || true' EXIT

HZ_CTL_PATH="$SOCK" $HZ tests/onfail.hs > "$LOGFILE" 2>&1 &
PID=$!
# Wait up to 8s for hoshizora to exit on its own via on-fail: shutdown
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
    if ! kill -0 $PID 2>/dev/null; then break; fi
    sleep 0.5
done
if kill -0 $PID 2>/dev/null; then
    echo "FAIL: hoshizora did not self-exit via on-fail: shutdown (still alive after 8s)"
    kill -9 $PID 2>/dev/null
    wait 2>/dev/null
    cat "$LOGFILE"
    rm -f "$LOGFILE"
    exit 1
fi
wait $PID 2>/dev/null

echo "--- assertions ---"
chk "loaded service flaky"            "flaky service parsed"
chk "respawn=1 max=1"                 "backoff(max=1) extracted"
chk ", on-fail"                       "on-fail: directive parsed"
chk "respawn scheduled in 1s"         "respawn timer armed after first crash"
chk "giving up after 1 restarts"      "backoff guard fired on second start attempt"
chk "on-fail: shutdown"               "on_fail_trigger fired (shutdown action)"
chk "shutdown signal received"        "g_shutdown propagated to main loop"

# Errors we expect in the sandbox (non-PID-1, no CAP_SYS_BOOT): reboot syscall
# fails. That's fine — on-fail still fired, main loop still exited. Don't treat
# as a test failure.
if grep -qE "parse error|FATAL|Assertion" "$LOGFILE"; then
    echo "FAIL: errors in log"
    FAIL=$((FAIL+1))
fi
rm -f "$LOGFILE"

summary "on-fail"
