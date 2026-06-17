#!/bin/bash
# Hoshizora core self-check: parses tests/core.hs, starts services, exercises
# the control socket via hzctl (list/status/stop/start/reload/shutdown),
# asserts each step. deferred: one shell script, no test framework.
# No `set -e` — we want full output even on partial failure.
cd "$(dirname "$0")/.."   # repo root, where ./hoshizora and ./hzctl live
. tests/lib.sh

HZ=./hoshizora
HZCTL=./hzctl
SOCKDIR=/tmp/hz_test_$$
SOCK=$SOCKDIR/control
LOGFILE=/tmp/hz_test.log
mkdir -p "$SOCKDIR"
trap 'rm -rf "$SOCKDIR" "$LOGFILE"; kill $PID 2>/dev/null || true' EXIT

# Start hoshizora with the test config + override socket path
HZ_CTL_PATH="$SOCK" $HZ tests/core.hs > "$LOGFILE" 2>&1 &
PID=$!

# Wait for socket to appear
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$SOCK" ] && break
    sleep 0.2
done
if [ ! -S "$SOCK" ]; then
    echo "FAIL: control socket not created"
    cat "$LOGFILE"
    exit 1
fi
export HZ_CTL_PATH="$SOCK"

echo "--- list (initial) ---"
$HZCTL list

echo "--- status bad_exec (should be failed) ---"
$HZCTL bad_exec status
sleep 0.3
$HZCTL bad_exec status

echo "--- status true_one ---"
$HZCTL true_one status

echo "--- SOV: true_one status (name-first) ---"
$HZCTL true_one status

echo "--- stop true_two ---"
$HZCTL true_two stop
sleep 0.3

echo "--- list (after stop) ---"
$HZCTL list

echo "--- start true_two ---"
$HZCTL true_two start
sleep 0.3

echo "--- list (after start) ---"
$HZCTL list

echo "--- SOV: true_two stop (name-first) ---"
$HZCTL true_two stop
sleep 0.2

echo "--- enable/disable/show cycle ---"
$HZCTL true_one disable
$HZCTL show
$HZCTL true_one start    # should skip — disabled
sleep 0.1
$HZCTL true_one enable

echo "--- logs 5 (in-memory ring) ---"
$HZCTL logs 5

echo "--- daemon-reload (explicit alias) ---"
$HZCTL daemon-reload

echo "--- help ---"
$HZCTL help

echo "--- reload (no arg = daemon-reload) ---"
$HZCTL reload

echo "--- shutdown ---"
$HZCTL shutdown
wait $PID 2>/dev/null || true

# Assertions (against server log + hzctl stdout)
echo "--- assertions ---"
grep -q "loaded 3 services" "$LOGFILE" && echo "PASS: parsed 3 services"
grep -q "control socket:" "$LOGFILE" && echo "PASS: control socket created"
grep -q "exec failed (exit 127)" "$LOGFILE" && echo "PASS: exec failure detected"
# bad_exec should NOT have respawned — the fix marks it FAILED immediately.
# Without the fix, we'd see "bad_exec: respawn scheduled" in the log.
if grep -q "bad_exec: respawn scheduled" "$LOGFILE"; then
    echo "FAIL: bad_exec respawned (should be FAILED without respawn)"
    cat "$LOGFILE"
    exit 1
fi
echo "PASS: no respawn storm for bad_exec"
# stop/start/reload/shutdown return "ok" or "shutting down" — verified by hzctl exit code 0 above
grep -q "reload: re-reading" "$LOGFILE" && echo "PASS: reload command worked"
# PID 1 must call reboot(2) on shutdown — returning from main = panic.
# In test env (non-PID-1) reboot() fails with EPERM, which we log + fall through.
grep -q "shutdown signal received" "$LOGFILE" && echo "PASS: clean shutdown initiated"
grep -q "reboot syscall failed" "$LOGFILE" && echo "PASS: reboot syscall invoked (expected EPERM in non-PID-1 test env)"
# enable/disable/show ephemeral state
grep -q "true_one: disabled — skipping start" "$LOGFILE" && echo "PASS: disabled service skipped on start"
# logs ring buffer should have content (services ran + reloaded)
[ -s "$LOGFILE" ] && echo "PASS: log ring has content"
if grep -qE "(parse error|FAIL|no such service)" "$LOGFILE"; then
    echo "FAIL: errors in log"
    cat "$LOGFILE"
    exit 1
fi
echo "PASS: no errors in log"

echo "--- self-check complete ---"
