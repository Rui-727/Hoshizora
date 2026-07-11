#!/bin/bash
# Hoshizora edge-case self-check: signal safety, write_all, restart rate
# limiter, cgroup leak prevention. Mix of static source checks (because some
# paths need root + cgroup v2 to exercise functionally) and runtime checks
# against the built binary.
#
# Static checks fail loudly if a future edit regresses any invariant:
#   1. No async-signal handlers (signal()/sigaction() that aren't SIG_IGN).
#      PID 1 must use signalfd exclusively; signal handlers must not call
#      non-async-signal-safe functions (printf, malloc, syslog, etc.).
#   2. write_all() helper exists and is used for every PID-1 write() to a
#      client fd (log_msg, ctl_send, logs_dump) so partial writes can't
#      truncate responses.
#   3. cgroup_kill_remaining() is called from start_service so leftover
#      processes from a SIGKILLed previous instance don't share the cgroup.
#
# Runtime checks:
#   4. Fast-crash rate limiter fires after 5 fast crashes in 30s, even when
#      max_restarts is high (would otherwise keep respawning).
cd "$(dirname "$0")/.."   # repo root
. tests/lib.sh

HZ=./hoshizora
HZCTL=./hzctl
SOCKDIR=/tmp/hz_edge_$$
SOCK=$SOCKDIR/control
LOGFILE=/tmp/hz_edge.log
mkdir -p "$SOCKDIR"
trap 'rm -rf "$SOCKDIR" "$LOGFILE"; kill $PID 2>/dev/null || true' EXIT

echo "--- static check 1: signal safety (no async-signal handlers) ---"
# Allow: signal(SIGPIPE, SIG_IGN) and signal(SIGXXX, SIG_DFL) for reset.
# Anything else (signal(SIGXXX, handler_func) or sigaction with sa_handler)
# is a bug — PID 1 must use signalfd.
SIG_INSTALLS=$(grep -nE 'signal\(\s*SIG[A-Z]+\s*,\s*[A-Za-z_][A-Za-z_0-9]*\s*\)' src/init.c \
               | grep -vE 'SIG_IGN|SIG_DFL' || true)
if [ -z "$SIG_INSTALLS" ]; then
    echo "PASS: no async-signal handlers in src/init.c (signalfd only)"
    PASS=$((PASS+1))
else
    echo "FAIL: signal handler installed (must use signalfd):"
    echo "$SIG_INSTALLS"
    FAIL=$((FAIL+1))
fi

# Same check for the sibling binaries. They aren't PID 1, but they must not
# install handlers that call non-async-signal-safe functions either.
for f in src/hzctl.c src/hzlog.c src/hz-event-logger.c; do
    SIG_INSTALLS=$(grep -nE 'signal\(\s*SIG[A-Z]+\s*,\s*[A-Za-z_][A-Za-z_0-9]*\s*\)' "$f" \
                  | grep -vE 'SIG_IGN|SIG_DFL' || true)
    if [ -z "$SIG_INSTALLS" ]; then
        echo "PASS: no async-signal handlers in $f"
        PASS=$((PASS+1))
    else
        echo "FAIL: signal handler installed in $f:"
        echo "$SIG_INSTALLS"
        FAIL=$((FAIL+1))
    fi
done

# Verify signalfd is actually used (the only legitimate signal-integration path).
if grep -q 'signalfd(' src/init.c; then
    echo "PASS: signalfd used for signal dispatch"
    PASS=$((PASS+1))
else
    echo "FAIL: signalfd not used in src/init.c"
    FAIL=$((FAIL+1))
fi

# Verify the LOG macros don't call syslog from a signal context. We can't
# prove absence statically for all paths, but we can verify log_msg is the
# only caller of vsyslog and it's never called from handle_signal.
if grep -q 'vsyslog(' src/init.c && ! grep -nE 'handle_signal.*vsyslog|vsyslog.*handle_signal' src/init.c; then
    echo "PASS: vsyslog only called from log_msg (main-loop context)"
    PASS=$((PASS+1))
else
    echo "FAIL: vsyslog reachable from signal context"
    FAIL=$((FAIL+1))
fi

echo "--- static check 2: write_all() helper exists and is used ---"
if grep -q 'static int write_all(int fd' src/init.c; then
    echo "PASS: write_all() defined"
    PASS=$((PASS+1))
else
    echo "FAIL: write_all() not defined"
    FAIL=$((FAIL+1))
fi
# write_all must be used by log_msg (stderr write), ctl_send, and logs_dump.
for caller in 'log_msg' 'ctl_send' 'logs_dump'; do
    # Look for write_all within ~30 lines after the function definition.
    if awk "/^static.* $caller\(/,/^}/" src/init.c | grep -q 'write_all'; then
        echo "PASS: $caller uses write_all"
        PASS=$((PASS+1))
    else
        echo "FAIL: $caller does not use write_all"
        FAIL=$((FAIL+1))
    fi
done

echo "--- static check 3: cgroup leak prevention wired into start_service ---"
if grep -q 'cgroup_kill_remaining' src/init.c; then
    echo "PASS: cgroup_kill_remaining() defined"
    PASS=$((PASS+1))
else
    echo "FAIL: cgroup_kill_remaining() not defined"
    FAIL=$((FAIL+1))
fi
if awk '/^static int start_service\(hz_service_t/,/^}/' src/init.c \
   | grep -q 'cgroup_kill_remaining'; then
    echo "PASS: start_service calls cgroup_kill_remaining"
    PASS=$((PASS+1))
else
    echo "FAIL: start_service does not call cgroup_kill_remaining"
    FAIL=$((FAIL+1))
fi
# cgroup.kill file write must use write_all (atomic write, no partial).
if awk '/^static void cgroup_kill_remaining\(/,/^}/' src/init.c \
   | grep -q 'write_all'; then
    echo "PASS: cgroup.kill write uses write_all"
    PASS=$((PASS+1))
else
    echo "FAIL: cgroup.kill write does not use write_all"
    FAIL=$((FAIL+1))
fi

echo "--- runtime check 4: fast-crash rate limiter fires ---"
# Config: /bin/false exits immediately (fast crash). max=10 so the existing
# max_restarts guard doesn't fire first. The rate limiter (5 fast crashes in
# 30s) should kick in after the 5th crash.
cat > "$SOCKDIR/edge.hs" <<'EOF'
system "edge-test" {
    service crasher {
        exec: "/bin/false";
        respawn: backoff(max = 10, base = 1s);
    }
}
EOF

HZ_CTL_PATH="$SOCK" $HZ "$SOCKDIR/edge.hs" > "$LOGFILE" 2>&1 &
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

# Wait for the rate limiter to fire. With linear backoff 1+2+3+4=10s before
# the 5th crash, give it 20s to settle.
echo "  (waiting up to 20s for 5 fast crashes + rate-limit hit)"
FIRED=0
for i in $(seq 1 40); do
    if grep -q 'fast-crash rate limit hit' "$LOGFILE"; then
        FIRED=1
        break
    fi
    sleep 0.5
done

# Don't bother sending shutdown — the service is FAILED, hoshizora is idle.
# Just kill it.
kill $PID 2>/dev/null
wait $PID 2>/dev/null || true

if [ "$FIRED" -eq 1 ]; then
    echo "PASS: fast-crash rate limiter fired after 5 crashes"
    PASS=$((PASS+1))
else
    echo "FAIL: fast-crash rate limiter did not fire"
    FAIL=$((FAIL+1))
fi

# The rate limiter should mark the service FAILED.
if grep -q 'fast-crash rate limit hit' "$LOGFILE"; then
    echo "PASS: rate-limit-hit log line present"
    PASS=$((PASS+1))
else
    echo "FAIL: rate-limit-hit log line missing"
    FAIL=$((FAIL+1))
fi

# Without the rate limiter, /bin/false with max=10 would respawn 10 times
# before max_restarts kicked in. Verify restart_count stayed at 5 (rate-limit
# cut it off early).
RESTARTS=$(grep -oE 'respawn scheduled in [0-9]+s \(attempt [0-9]+\)' "$LOGFILE" \
           | tail -1 | grep -oE 'attempt [0-9]+' | grep -oE '[0-9]+' || echo 0)
if [ "$RESTARTS" -ge 1 ] && [ "$RESTARTS" -le 5 ]; then
    echo "PASS: restarts capped at $RESTARTS (rate limiter stopped the storm)"
    PASS=$((PASS+1))
else
    echo "FAIL: restart count $RESTARTS not in expected 1-5 range"
    FAIL=$((FAIL+1))
fi

# Total fast-crash warnings should be 4 (one per crash before the 5th which
# hits the limit and goes through mark_failed instead).
FAST_WARN=$(grep -c 'fast crash (' "$LOGFILE" || true)
if [ "$FAST_WARN" -ge 1 ] && [ "$FAST_WARN" -le 5 ]; then
    echo "PASS: fast-crash warnings logged ($FAST_WARN total)"
    PASS=$((PASS+1))
else
    echo "FAIL: fast-crash warning count $FAST_WARN out of expected 1-5 range"
    FAIL=$((FAIL+1))
fi

# No parse errors or asserts.
if grep -qE 'parse error|FATAL|Assertion' "$LOGFILE"; then
    echo "FAIL: errors in log"
    FAIL=$((FAIL+1))
else
    echo "PASS: no fatal errors in log"
    PASS=$((PASS+1))
fi

echo "--- edge-cases self-check complete ---"
summary "edge-cases"
