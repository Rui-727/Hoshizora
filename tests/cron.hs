# Hoshizora cron self-check config — used by tests/cron.sh.
# A cron job scheduled every 2s that touches a marker file. After 5s we expect
# the file to exist (job fired at least once).
#
# deferred: 2s interval — short enough that the test runs fast, long enough
# that the timerfd arming + firing path is exercised properly.
system "cron-test" {
    service marker {
        exec: "/bin/sh" with args ["-c", "touch /tmp/hz_cron_marker"];
        every: "2s";
    }
}
