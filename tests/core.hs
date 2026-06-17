# Hoshizora self-check config — uses /bin/true so it works on any Linux.
# Tests: parsing, fork+exec, dependency ordering, clean exit on SIGTERM,
#         exec-failure handling (no respawn storm).
system "self-check" {
    service true_one {
        exec: "/bin/true";
        respawn: backoff(max = 3, base = 1s);
        environment: {
            "HZ_TEST": "one"
        };
    }

    service true_two {
        requires: [true_one];
        exec: "/bin/true";
        respawn: backoff(max = 3, base = 1s);
    }

    # ponytail: exec-failure test — bad path, child exits 127. Should go to
    # FAILED without respawning. Without the fix, would respawn 3x instantly.
    service bad_exec {
        exec: "/nonexistent/path";
        respawn: backoff(max = 3, base = 1s);
    }
}
