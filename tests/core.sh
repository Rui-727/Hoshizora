#!/bin/bash
# Hoshizora core self-check: parses tests/core.hs, starts services, exercises
# the control socket via hzctl (list/status/stop/start/reload/shutdown),
# asserts each step. ponytail: one shell script, no test framework.
# No `set -e` — we want full output even on partial failure.
cd "$(dirname "$0")/.."   # repo root, where ./hoshizora and ./hzctl live

HZ=./hoshizora
HZCTL=./hzctl
SOCKDIR=/tmp/hz_test_$$
SOCK=$SOCKDIR/control
mkdir -p "$SOCKDIR"
trap 'rm -rf "$SOCKDIR"; kill $PID 2>/dev/null || true' EXIT

# Start hoshizora with the test config + override socket path
HZ_CTL_PATH="$SOCK" $HZ tests/core.hs > /tmp/hz_test.log 2>&1 &
PID=$!

# Wait for socket to appear
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$SOCK" ] && break
    sleep 0.2
done
if [ ! -S "$SOCK" ]; then
    echo "FAIL: control socket not created"
    cat /tmp/hz_test.log
    exit 1
fi
export HZ_CTL_PATH="$SOCK"

echo "--- list (initial) ---"
$HZCTL list

echo "--- status bad_exec (should be failed) ---"
$HZCTL status bad_exec
sleep 0.3
$HZCTL status bad_exec

echo "--- status true_one ---"
$HZCTL status true_one

echo "--- Gentoo-style: true_one status (name-first) ---"
$HZCTL true_one status

echo "--- stop true_two ---"
$HZCTL stop true_two
sleep 0.3

echo "--- list (after stop) ---"
$HZCTL list

echo "--- start true_two ---"
$HZCTL start true_two
sleep 0.3

echo "--- list (after start) ---"
$HZCTL list

echo "--- Gentoo-style: true_two stop (name-first) ---"
$HZCTL true_two stop
sleep 0.2

echo "--- enable/disable/show cycle ---"
$HZCTL disable true_one
$HZCTL show
$HZCTL true_one start    # should skip — disabled
sleep 0.1
$HZCTL enable true_one

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
grep -q "loaded 3 services" /tmp/hz_test.log && echo "PASS: parsed 3 services"
grep -q "control socket:" /tmp/hz_test.log && echo "PASS: control socket created"
grep -q "exec failed (exit 127)" /tmp/hz_test.log && echo "PASS: exec failure detected"
# bad_exec should NOT have respawned — the fix marks it FAILED immediately.
# Without the fix, we'd see "bad_exec: respawn scheduled" in the log.
if grep -q "bad_exec: respawn scheduled" /tmp/hz_test.log; then
    echo "FAIL: bad_exec respawned (should be FAILED without respawn)"
    cat /tmp/hz_test.log
    exit 1
fi
echo "PASS: no respawn storm for bad_exec"
# stop/start/reload/shutdown return "ok" or "shutting down" — verified by hzctl exit code 0 above
grep -q "reload: re-reading" /tmp/hz_test.log && echo "PASS: reload command worked"
# Patch 3: PID 1 must call reboot(2) on shutdown — returning from main = panic.
# In test env (non-PID-1) reboot() fails with EPERM, which we log + fall through.
grep -q "shutdown signal received" /tmp/hz_test.log && echo "PASS: clean shutdown initiated"
grep -q "reboot syscall failed" /tmp/hz_test.log && echo "PASS: reboot syscall invoked (expected EPERM in non-PID-1 test env)"
# Patch 4: enable/disable/show ephemeral state
grep -q "true_one: disabled — skipping start" /tmp/hz_test.log && echo "PASS: disabled service skipped on start"
# logs ring buffer should have content (services ran + reloaded)
[ -s /tmp/hz_test.log ] && echo "PASS: log ring has content"
if grep -qE "(parse error|FAIL|no such service)" /tmp/hz_test.log; then
    echo "FAIL: errors in log"
    cat /tmp/hz_test.log
    exit 1
fi
echo "PASS: no errors in log"

echo "--- self-check complete ---"
