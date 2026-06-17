#!/bin/bash
# hzlog self-check: bind /dev/log (overridden), send a syslog line via logger(1)
# or sendto(2), assert it appears in the log file. ponytail: one line in, one
# line out, done.
cd "$(dirname "$0")/.."   # repo root, where ./hzlog lives

HZLOG=./hzlog
SOCKDIR=/tmp/hz_log_$$
SOCK=$SOCKDIR/log
LOGFILE=/tmp/hz_log_out_$$
MARKER="hzlog_selfcheck_$$"
mkdir -p "$SOCKDIR"
rm -f "$LOGFILE"
trap 'rm -rf "$SOCKDIR" "$LOGFILE"; kill $PID 2>/dev/null || true' EXIT

HZ_LOG_SOCK="$SOCK" HZ_LOG_FILE="$LOGFILE" $HZLOG > /tmp/hz_log_stderr.log 2>&1 &
PID=$!
# Wait for socket to appear
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$SOCK" ] && break
    sleep 0.1
done
if [ ! -S "$SOCK" ]; then
    echo "FAIL: hzlog did not create socket"
    cat /tmp/hz_log_stderr.log
    exit 1
fi

echo "--- sending syslog datagram via python (no logger(1) dependency) ---"
python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.connect('$SOCK')
s.sendall(b'<13>hzlog selfcheck: $MARKER')
s.close()
"

# Wait for the line to appear in the log file
FOUND=0
for i in 1 2 3 4 5 6 7 8 9 10; do
    if [ -f "$LOGFILE" ] && grep -q "$MARKER" "$LOGFILE"; then
        FOUND=1
        break
    fi
    sleep 0.1
done

kill $PID 2>/dev/null
wait $PID 2>/dev/null || true

echo "--- assertions ---"
PASS=0
FAIL=0

if [ "$FOUND" -eq 1 ]; then
    echo "PASS: syslog line appeared in log file"
    PASS=$((PASS+1))
else
    echo "FAIL: syslog line NOT found in log file"
    FAIL=$((FAIL+1))
    echo "--- log file contents ---"
    cat "$LOGFILE" 2>/dev/null || echo "(empty or missing)"
    echo "--- hzlog stderr ---"
    cat /tmp/hz_log_stderr.log 2>/dev/null
fi

if grep -q "listening on" /tmp/hz_log_stderr.log; then
    echo "PASS: hzlog started up"
    PASS=$((PASS+1))
else
    echo "FAIL: hzlog did not announce startup"
    FAIL=$((FAIL+1))
fi

rm -f /tmp/hz_log_stderr.log

echo "--- hzlog self-check: $PASS PASS, $FAIL FAIL ---"
[ "$FAIL" -eq 0 ] || exit 1
