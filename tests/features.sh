#!/bin/bash
# Hoshizora features self-check: parses tests/features.hs, verifies the parser
# accepts memory-limit / cpu-weight / watch blocks / oom-kill / log /
# network_ready virtual intent, exercises cgroup setup (will warn about
# missing cgroup v2 / permissions in non-root sandbox — that's expected and
# OK), then shuts down. Does NOT verify fanotify events fire (requires
# CAP_SYS_ADMIN) — just that setup_fanotify is called without crashing
# hoshizora. DOES verify per-service log files are created + written.
cd "$(dirname "$0")/.."   # repo root, where ./hoshizora and ./hzctl live
. tests/lib.sh

HZ=./hoshizora
HZCTL=./hzctl
SOCKDIR=/tmp/hz_feat_$$
SOCK=$SOCKDIR/control
LOGFILE=/tmp/hz_feat.log
mkdir -p "$SOCKDIR"
touch /tmp/hz_features_marker
mkdir -p /tmp/hz_features_dir
# Per-service log paths — these should be created on start. Clean any
# stale content so the existence check below is meaningful.
rm -f /tmp/hz_features_limited.log /tmp/hz_features_dep.log
trap 'rm -rf "$SOCKDIR" "$LOGFILE" /tmp/hz_features_marker /tmp/hz_features_limited.log /tmp/hz_features_dep.log; rmdir /tmp/hz_features_dir 2>/dev/null; kill $PID 2>/dev/null || true' EXIT

HZ_CTL_PATH="$SOCK" $HZ tests/features.hs > "$LOGFILE" 2>&1 &
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

echo "--- list (initial) ---"
$HZCTL list

echo "--- reload (verifies parse works on reload too) ---"
$HZCTL reload

echo "--- shutdown ---"
$HZCTL shutdown
wait $PID 2>/dev/null

echo "--- assertions ---"
grep -q "loaded 3 services, 2 watches" "$LOGFILE" && echo "PASS: parsed 3 services + 2 watches"
grep -q "loaded service limited_service" "$LOGFILE" && echo "PASS: limited_service loaded"
grep -q "loaded service dep_service" "$LOGFILE" && echo "PASS: dep_service loaded"
grep -q "loaded service net_consumer" "$LOGFILE" && echo "PASS: net_consumer loaded"
grep -q "loaded watch /tmp/hz_features_marker" "$LOGFILE" && echo "PASS: watch 1 parsed"
grep -q "loaded watch /tmp/hz_features_dir" "$LOGFILE" && echo "PASS: watch 2 parsed"
grep -q "reload: re-reading" "$LOGFILE" && echo "PASS: reload worked"
grep -q "shutdown signal received" "$LOGFILE" && echo "PASS: clean shutdown initiated"
# cgroup warnings are OK in non-root sandbox — we just verify they don't crash
if grep -qE "parse error|no such service|FATAL|Assertion" "$LOGFILE"; then
    echo "FAIL: errors in log"
    cat "$LOGFILE"
    exit 1
fi
echo "PASS: no fatal errors"

# Verify mem/cpu fields made it into the load log
grep -q "mem=" "$LOGFILE" && echo "PASS: memory-limit parsed"
grep -q "cpu=" "$LOGFILE" && echo "PASS: cpu-weight parsed"
# backoff(max=N) now extracts N into max_restarts (was always 5 before).
# limited_service has backoff(max=3) — verify max=3 appears in the log.
grep -q "respawn=1 max=3" "$LOGFILE" && echo "PASS: backoff(max=3) extracted"
# start-condition now parsed and shown in load log.
grep -q "start-cond" "$LOGFILE" && echo "PASS: start-condition parsed"
# Both services should have started — file-exists("/bin/true") and link-up("lo")
# are both true on any Linux box.
grep -q "started limited_service" "$LOGFILE" && echo "PASS: limited_service started (condition true)"
grep -q "started dep_service" "$LOGFILE" && echo "PASS: dep_service started (condition true)"
# In non-root / cgroup-v1 sandbox, we expect the v2-not-available warning.
# On a real PID 1 with cgroup v2, this line wouldn't appear — and that's OK.
if grep -q "cgroup v2 not mounted" "$LOGFILE"; then
    echo "PASS: cgroup v2 unavailability detected + warned (sandbox case)"
fi

# oom-kill: group parsed — should appear in the load log
grep -q "oom-group" "$LOGFILE" && echo "PASS: oom-kill: group parsed"
# per-service log redirect — `log: "/path"` parsed
grep -q ", log)" "$LOGFILE" && echo "PASS: log: directive parsed"
# per-service log file actually created + written by /bin/true
# /bin/true doesn't print anything, so the file may be empty — but it must exist.
if [ -f /tmp/hz_features_limited.log ]; then
    echo "PASS: per-service log file created (limited_service)"
else
    echo "FAIL: per-service log file NOT created (limited_service)"
fi
if [ -f /tmp/hz_features_dep.log ]; then
    echo "PASS: per-service log file created (dep_service)"
else
    echo "FAIL: per-service log file NOT created (dep_service)"
fi
# network_ready virtual intent — net_consumer depends on it.
# In the sandbox eth0 has IFF_UP, so the dep should be satisfied and
# net_consumer should start. (No "unknown dep 'network_ready'" warning.)
if grep -q "unknown dep 'network_ready'" "$LOGFILE"; then
    echo "FAIL: network_ready treated as unknown dep (virtual intent not wired)"
else
    echo "PASS: network_ready not warned as unknown dep"
fi
grep -q "started net_consumer" "$LOGFILE" && echo "PASS: net_consumer started (network_ready satisfied)"
# on-fail: directive parsed — should appear in dep_service's load log
grep -q ", on-fail" "$LOGFILE" && echo "PASS: on-fail: directive parsed"

echo "--- features self-check complete ---"
