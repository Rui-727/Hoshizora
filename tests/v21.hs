# Hoshizora v2.1 features self-check config.
# Tests: plugin event broadcast socket + shutdown gate.
system "v21-test" {
    service simple {
        exec: "/bin/true";
        respawn: backoff(max = 3);
    }
}
