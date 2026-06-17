# Hoshizora v2.0 features self-check config.
# Tests: variable substitution, timeout-start, listen (socket activation), target.
# deferred: sd_notify + container integration need root + custom binary.
$SOCK = "/tmp/hz_v2_listen.sock";

system "v2-test" {
    service quitter {
        exec: "/bin/true";
        start-condition: file-exists("/bin/true");
        timeout-start: "5s";
    }

    service listener {
        exec: "/bin/true";
        listen: "$SOCK";
        timeout-start: "5s";
    }

    target v2-target {
        requires: [quitter, listener];
    }
}
