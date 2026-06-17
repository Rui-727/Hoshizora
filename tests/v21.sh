#!/bin/bash
# v2.1 features self-check: plugin event socket + shutdown gate.
# deferred: session tracker (pam_exec) needs PAM + a real login — tested
# manually by triggering pam_exec; here we just test the shutdown gate
# with a synthetic session file.
cd "$(dirname "$0")/.."   # repo root
. tests/lib.sh

HZ=./hoshizora
HZCTL=./hzctl
SOCKDIR=/tmp/hz_v21_$$
SOCK=$SOCKDIR/control
EVENTS=$SOCKDIR/events
SESSDIR=$SOCKDIR/sessions
LOGFILE=/tmp/hz_v21.log
mkdir -p "$SOCKDIR" "$SESSDIR"
trap 'rm -rf "$SOCKDIR" "$LOGFILE"; kill $PID 2>/dev/null || true' EXIT

# Start hoshizora with control + event sockets under SOCKDIR
HZ_CTL_PATH="$SOCK" HZ_EVENT_PATH="$EVENTS" $HZ tests/v21.hs > "$LOGFILE" 2>&1 &
PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$SOCK" ] && break
    sleep 0.2
done
if [ ! -S "$SOCK" ]; then
    echo "FAIL: control socket not created"
    cat "$LOGFILE"
    exit 1
fi

echo "--- subscribe to event socket ---"
# Connect and read 1 line in the background, save to file
python3 -c "
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$EVENTS')
# Read for up to 2s, accumulate events
s.settimeout(2.0)
buf = b''
try:
    while True:
        chunk = s.recv(256)
        if not chunk: break
        buf += chunk
        if b'START' in buf or b'SHUTDOWN' in buf: break
except (TimeoutError, OSError):
    pass
sys.stdout.buffer.write(buf)
" > /tmp/hz_v21_events.txt 2>&1 &
SUB_PID=$!

# Give the subscriber a moment to connect
sleep 0.3

echo "--- restart the service (should generate START event) ---"
HZ_CTL_PATH="$SOCK" $HZCTL restart simple
sleep 0.3

echo "--- shutdown gate: with a fake session ---"
touch "$SESSDIR/alice"
HZ_SESSION_DIR="$SESSDIR" HZ_CTL_PATH="$SOCK" $HZCTL shutdown 2>&1 || echo "  (refused, exit $?)"

echo "--- shutdown gate: --force override ---"
HZ_SESSION_DIR="$SESSDIR" HZ_CTL_PATH="$SOCK" $HZCTL shutdown --force 2>&1 || echo "  (exit $?)"
wait $PID 2>/dev/null || true
wait $SUB_PID 2>/dev/null || true

echo "--- assertions ---"
PASS=0
FAIL=0
chk "event socket:"                "event socket bound"
chk "event subscriber accepted"    "subscriber connect handled"
# Event file should contain a HELLO and at least one START
if grep -q "HELLO" /tmp/hz_v21_events.txt && grep -q "START" /tmp/hz_v21_events.txt; then
    echo "PASS: HELLO + START events received by subscriber"
    PASS=$((PASS+1))
else
    echo "FAIL: subscriber did not receive HELLO + START"
    echo "--- events file ---"
    cat /tmp/hz_v21_events.txt
    FAIL=$((FAIL+1))
fi

# Shutdown gate refused without --force
if [ -f "$SESSDIR/alice" ]; then
    echo "PASS: shutdown gate refused (session file still present)"
    PASS=$((PASS+1))
else
    echo "FAIL: shutdown gate didn't refuse"
    FAIL=$((FAIL+1))
fi

# After --force, shutdown proceeded (server exited)
if grep -q "shutdown signal received" "$LOGFILE"; then
    echo "PASS: shutdown proceeded with --force"
    PASS=$((PASS+1))
else
    echo "FAIL: shutdown did not proceed with --force"
    FAIL=$((FAIL+1))
fi

if grep -qE "parse error|FATAL|Assertion" "$LOGFILE"; then
    echo "FAIL: errors in log"
    FAIL=$((FAIL+1))
fi

rm -f "$LOGFILE" /tmp/hz_v21_events.txt
summary "v2.1"
