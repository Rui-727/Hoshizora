# Hoshizora on-fail runtime test config — used by tests/onfail.sh.
# A flaky service with backoff(max=1) and on-fail: shutdown. After 1 respawn
# attempt fails, on_fail_trigger fires → g_shutdown=1 → hoshizora exits.
# The test asserts "on-fail: shutdown" appears in the log.
#
# /bin/false exits 1 immediately, so it crashes on every start. With
# max_restarts=1 the second start attempt hits the backoff guard in
# start_service (line ~923) → mark_failed → on_fail_trigger.
system "onfail-test" {
    service flaky {
        exec: "/bin/false";
        respawn: backoff(max = 1, base = 1s);
        on-fail: shutdown;
    }
}
