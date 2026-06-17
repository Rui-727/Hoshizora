# Hoshizora features-test config — exercises the parser paths for cgroups +
# fanotify watches + start-condition + healthy + backoff(max=N) + oom-kill +
# per-service log + network_ready virtual intent without needing root or
# CAP_SYS_ADMIN. Used by tests/features.sh. The runtime cgroup/fanotify/
# health paths are exercised when run as PID 1; here we just verify the parser
# accepts these fields.
system "features-test" {
    service limited_service {
        exec: "/bin/true";
        respawn: backoff(max = 3, base = 1s);
        memory-limit: 256MiB;
        cpu-weight: 50;
        oom-kill: group;
        log: "/tmp/hz_features_limited.log";
        start-condition: file-exists("/bin/true");
        environment: {
            "HZ_TEST": "limited"
        };
    }

    service dep_service {
        requires: [limited_service];
        exec: "/bin/true";
        memory-limit: 1GiB;
        cpu-weight: 80;
        log: "/tmp/hz_features_dep.log";
        # AND-of-two-builtins form — both must be true to start.
        # link-up("lo") is true on any Linux box with networking.
        start-condition: file-exists("/bin/true") and link-up("lo");
        # ponytail: on-fail: restart(limited_service) — if dep_service goes
        # FAILED, restart its dep (limited_service) too. Parser-only check here;
        # runtime firing is exercised by tests/onfail.sh.
        on-fail: restart(limited_service);
    }

    # ponytail: network_ready is a VIRTUAL intent — no service block defines
    # it. Without the virtual-dep code, this would log "unknown dep" and skip
    # the check. With it, the dep is satisfied (sandbox has eth0 IFF_UP).
    service net_consumer {
        requires: [network_ready];
        exec: "/bin/true";
    }

    # Watch directives — parsed, will be inert without CAP_SYS_ADMIN.
    watch "/tmp/hz_features_marker" {
        on-change: reload(limited_service);
    }

    watch "/tmp/hz_features_dir" recursive {
        on-change: restart(dep_service);
    }
}
