#!/bin/bash
# Hoshizora cron self-check: every: directive fires a job at intervals.
# ponytail: one job, one marker file, one assertion — file exists after 5s.
cd "$(dirname "$0")/.."   # repo root, where ./hoshizora lives

HZ=./hoshizora
HZCTL=./hzctl
SOCKDIR=/tmp/hz_cron_$$
SOCK=$SOCKDIR/control
mkdir -p "$SOCKDIR"
rm -f /tmp/hz_cron_marker
trap 'rm -rf "$SOCKDIR" /tmp/hz_cron_marker; kill $PID 2>/dev/null || true' EXIT

HZ_CTL_PATH="$SOCK" $HZ tests/cron.hs > /tmp/hz_cron.log 2>&1 &
PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$SOCK" ] && break
    sleep 0.2
done
if [ ! -S "$SOCK" ]; then
    echo "FAIL: control socket not created"
    cat /tmp/hz_cron.log
    exit 1
fi
export HZ_CTL_PATH="$SOCK"

echo "--- list (initial) ---"
$HZCTL list

# Wait for at least one cron fire (interval=2s, so 4s is generous)
echo "--- waiting up to 6s for cron to fire ---"
FIRED=0
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    if [ -f /tmp/hz_cron_marker ]; then FIRED=1; break; fi
    sleep 0.5
done

echo "--- shutdown ---"
$HZCTL shutdown
wait $PID 2>/dev/null || true

echo "--- assertions ---"
PASS=0
FAIL=0
chk() {
    if grep -q "$1" /tmp/hz_cron.log; then
        echo "PASS: $2"
        PASS=$((PASS+1))
    else
        echo "FAIL: $2"
        FAIL=$((FAIL+1))
    fi
}

chk "loaded service marker"            "cron service parsed"
chk "cron scheduled, first fire in 2s" "cron job scheduled at startup"
chk "cron firing"                      "cron timer fired"
chk "cron re-armed, next fire in 2s"   "cron re-armed after clean exit"

if [ "$FIRED" -eq 1 ]; then
    echo "PASS: marker file created (cron job ran)"
    PASS=$((PASS+1))
else
    echo "FAIL: marker file NOT created after 6s"
    FAIL=$((FAIL+1))
fi

if grep -qE "parse error|FATAL|Assertion" /tmp/hz_cron.log; then
    echo "FAIL: errors in log"
    FAIL=$((FAIL+1))
fi
rm -f /tmp/hz_cron.log

echo "--- cron self-check: $PASS PASS, $FAIL FAIL ---"
[ "$FAIL" -eq 0 ] || exit 1
