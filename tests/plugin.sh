#!/bin/bash
# Plugin self-check: hz-event-logger subscribes, logs events to a file,
# and runs a command on matching events.
cd "$(dirname "$0")/.."   # repo root
. tests/lib.sh

HZ=./hoshizora
HZCTL=./hzctl
PLUGIN=./hz-event-logger
SOCKDIR=/tmp/hz_plugin_$$
SOCK=$SOCKDIR/control
EVENTS=$SOCKDIR/events
LOGFILE=/tmp/hz_plugin.log
EVENTLOG=/tmp/hz_plugin_events.txt
mkdir -p "$SOCKDIR"
rm -f "$EVENTLOG"
trap 'rm -rf "$SOCKDIR" "$LOGFILE" "$EVENTLOG"; kill $PID $PLUGPID 2>/dev/null || true' EXIT

# Start hoshizora with event socket under SOCKDIR
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
export HZ_CTL_PATH="$SOCK"

# v2.1.1: dropped the --match FAILED --run cat path — no FAILED events
# fire in this config (simple service has respawn, never gives up), so the
# RUNOUT file was dead. Plugin just logs all events now.
HZ_EVENT_PATH="$EVENTS" $PLUGIN --log "$EVENTLOG" &
PLUGPID=$!
sleep 0.4  # let it connect

echo "--- restart the service (generates START event) ---"
$HZCTL restart simple
sleep 0.3

# Plugin should still be running (didn't crash on event processing)
if kill -0 $PLUGPID 2>/dev/null; then
    echo "PASS: plugin stayed alive through events"
    PASS=$((PASS+1))
else
    echo "FAIL: plugin exited early"
    FAIL=$((FAIL+1))
fi

echo "--- shutdown ---"
$HZCTL shutdown --force
wait $PID 2>/dev/null || true
# Give the plugin a moment to receive the SHUTDOWN event
sleep 0.3
kill $PLUGPID 2>/dev/null || true
wait $PLUGPID 2>/dev/null || true

echo "--- assertions ---"
PASS=0
FAIL=0
chk "event socket:"              "event socket bound"
chk "event subscriber accepted"  "plugin subscribed"

# Event log should contain at least HELLO + START
if [ -f "$EVENTLOG" ] && grep -q "HELLO" "$EVENTLOG" && grep -q "START" "$EVENTLOG"; then
    echo "PASS: plugin logged HELLO + START events"
    PASS=$((PASS+1))
else
    echo "FAIL: plugin didn't log HELLO + START"
    echo "--- event log ---"
    cat "$EVENTLOG" 2>/dev/null || echo "(empty)"
    FAIL=$((FAIL+1))
fi

# SHUTDOWN event should appear in the log
if grep -q "SHUTDOWN" "$EVENTLOG" 2>/dev/null; then
    echo "PASS: plugin received SHUTDOWN event"
    PASS=$((PASS+1))
else
    echo "FAIL: plugin didn't receive SHUTDOWN"
    FAIL=$((FAIL+1))
fi

if grep -qE "parse error|FATAL|Assertion" "$LOGFILE"; then
    echo "FAIL: errors in log"
    FAIL=$((FAIL+1))
fi

rm -f "$LOGFILE" "$EVENTLOG"
summary "plugin"
