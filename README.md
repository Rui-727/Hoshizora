# Hoshizora (星宙 — "Starry Void")

A minimal init system (PID 1) for Linux/x86_64 with a runtime control socket.

## Design principles

- **Modular** — features that can be compiled out or disabled at runtime.
  The core (`init.c`) is a single poll() loop; cgroups, fanotify, health
  probes, cron scheduling, socket activation, sd-notify, and container
  namespaces are independent code paths that no-op cleanly when their
  kernel feature is missing or unavailable.
- **Transparent** — readable code, predictable behavior. No threads, no
  async runtime, no event-loop library. One process, one loop, six fds.
  Every runtime decision is logged with the service name and reason.
- **Minimal by default** — the core ships only what a real config needs.
  Optional features are tracked in `DEFERRED.md` and added when a concrete
  use case arrives, not speculatively.

See `ROADMAP.md` for the v2.0 feature set (shipped) and forward-looking
items still on the deferral list.

## What it does

1. Reads a `system.hs` config file (default `/etc/hoshizora/system.hs`, override via `argv[1]`).
2. Parses `service NAME { ... }` blocks, extracting: `exec`, `with args`, `requires`, `respawn` (with `backoff(max=N)`), `environment`, `memory-limit`, `cpu-weight`, `oom-kill`, `log`, `start-condition`, `healthy`, `on-fail`, `every`, `timeout-start`, `retry-after`, `expect-notify`, `listen`, `namespace`, `rootfs`, `bind`.
3. Parses `watch "path" [recursive] { on-change: reload(X) | restart(X); }` blocks.
4. Parses `target NAME { requires: [a, b, c]; }` blocks — named service groups for `hzctl start <target>`.
5. Parses top-level `$NAME = "value";` variable assignments, substituted into string-typed service fields (exec, log, listen, rootfs, bind).
6. Starts services in dependency order (recursive — deps started first), in parallel. **Virtual intent `network_ready`** is auto-satisfied when any non-`lo` interface has IFF_UP set (so `requires: [network_ready]` works without a dummy service).
7. Restarts crashed services with linear backoff (`restart_count` seconds, capped at 30) up to `max` times.
8. **Treats `execve` failures (child exit 127) as non-respawnable by default** — marks the service FAILED immediately, no respawn storm. Services with `start-condition:` instead re-arm `cron_next = now + retry-after` (default 5s) up to `max_restarts` times, treating the failure as transient (e.g. filesystem not mounted yet).
9. **Resets the signal mask in forked children** — parent blocks SIGCHLD/SIGTERM/SIGINT/SIGHUP for signalfd; children get a clean empty mask so `kill <pid>` works.
10. Exposes a Unix-domain control socket at `/run/hoshizora/control` (override via `HZ_CTL_PATH` env var).
11. Reloads config via SIGHUP or the `reload` command — diffs old/new, adopts unchanged running pids, **parallel-stops** changed services (O(5s) total), restarts changed, kills removed, starts new.
12. Handles `SIGTERM`/`SIGINT` for clean shutdown — **parallel stop** (SIGTERM all running at once, 5s wait, SIGKILL stragglers) so shutdown is O(5s) not O(N×5s). Then calls `reboot(2)` (RB_POWER_OFF or RB_AUTOBOOT) — PID 1 returning from `main()` would panic the kernel.
13. **Enforces per-service memory/cpu limits via cgroup v2** — creates `/sys/fs/cgroup/hoshizora/<name>/`, writes `memory.max` + `cpu.weight` + (optionally) `memory.oom.group`, assigns child pid to `cgroup.procs`. No-op with one-time warning if cgroup v2 isn't mounted.
14. **Watches files via fanotify** — on change to a watched path, dispatches `reload(X)` (sends SIGHUP to service X) or `restart(X)` (stop + start). Needs `CAP_SYS_ADMIN`; warns and disables watches if unavailable.
15. **Evaluates `start-condition:`** before starting — 3 builtins (`file-exists`, `link-up`, `fs-mounted`), combinable with `and` (max 2). Service stays STOPPED if condition is false; re-evaluated on each `start` so the operator can retry.
16. **Health-probes running services** every 5s via `tcp-probe("host:port", Ns)` — 3 consecutive failures mark the service FAILED and SIGTERM it. Respawn logic kicks in if `respawn:` is set.
17. **In-memory log ring** (8 KiB) — `hzctl logs [N]` dumps the last N lines. Survives until shutdown. Also forwarded to syslog via `openlog()`/`syslog()` for persistent collection by `hzlog` or any syslog daemon.
18. **Ephemeral enable/disable state** — `disable X` writes a marker file under `<ctl-dir>/enabled/`; `start X` skips while disabled. Survives reload, not reboot. For persistent state, edit the config.
19. **Per-service log redirect** — `log: "/path/to/file.log"` directive redirects the service's stdout+stderr to that file (O_APPEND, so restarts don't clobber). Empty/absent = inherit hoshizora's stderr.
20. **`oom-kill: group`** — writes `memory.oom.group=1` in the service cgroup so the kernel kills every process in the cgroup on OOM, not just the allocator. Prevents half-dead services.
21. **`on-fail:` action** — when a service transitions to FAILED (exec-fail, backoff exhausted, fork failure, crash without respawn), fire the configured action. Two builtins: `restart(other)` (stop+start target service) or `shutdown` (set g_shutdown=1, main loop exits cleanly). No cycle guard — operator's config, their bug to see in logs.
22. **`every:` cron-style scheduling** — a service with `every: "1h"` (or `"300s"`, `"5m"`) is a one-shot cron job. Scheduled via the existing timerfd; re-arms on clean exit. Crashed cron jobs fall through to the existing FAILED/respawn path.
23. **Drops capabilities after setup** — `capset(2)` keeps only `CAP_SYS_ADMIN` (cgroups, pivot_root), `CAP_KILL` (signaling), `CAP_SYS_BOOT` (reboot), `CAP_NET_ADMIN` (socket activation on privileged ports). All others dropped. Warns and continues with full caps if capset fails (e.g. sandbox without CAP_SETPCAP).
24. **Startup timeouts** — `timeout-start: "30s"` arms a deadline at `start_service` time. Checked in the 5s health tick. Disarmed on RUNNING, or on `READY=1` if `expect-notify: true;`. Missed deadline → SIGTERM + `mark_failed` + `on-fail:`.
25. **Socket activation** — `listen: "host:port";` (TCP) or `listen: "/path.sock";` (Unix). Hoshizora binds at startup, child inherits fds at 3+ via `HZ_LISTEN_FDS=N` env var. Lets services lazy-start on first connection without losing the socket.
26. **sd-notify readiness protocol** — `expect-notify: true;` configures a service to send `READY=1` over the global `/run/hoshizora/notify` socket (override via `HZ_NOTIFY_PATH`). Hoshizora parses `<service-name> READY=1` / `STOPPING=1`, disarms `start_deadline` on READY.
27. **Container integration** — `namespace: private;` calls `unshare(CLONE_NEWNS|NEWNET|NEWPID)` in the child post-fork; `rootfs: "/path";` bind-mounts and `pivot_root`s; `bind: "src:dst";` creates mount entries. All degrade to non-fatal warnings if capabilities are missing.
28. **Service targets** — `target NAME { requires: [a, b, c]; }` defines a named group. `hzctl start <target>` walks the list and starts each member. deferred: `isolate` (stop everything not in dep closure) — add when a real config needs it.

## Control socket protocol

Text protocol, one connection per command. **Both verb orders work** — action-first (`hzctl start nginx`) or name-first Gentoo-style (`hzctl nginx start`). The `hzctl` client (built alongside `hoshizora`) is a dumb pass-through; the server reorders as needed.

```bash
# Action-first (systemd-ish) — both forms work for all commands:
hzctl list                       # list all services + state
hzctl status [name]              # status of one or all services
hzctl start <name>
hzctl stop <name>
hzctl restart <name>
hzctl reload [<name>]            # no arg = daemon-reload; arg = SIGHUP service
hzctl daemon-reload              # explicit alias for re-reading config
hzctl enable <name>              # mark for autostart (ephemeral)
hzctl disable <name>             # skip at boot / reload (ephemeral)
hzctl show                       # list services + enabled state (rc-update show flavor)
hzctl logs [N]                   # last N log lines (default 50)
hzctl shutdown | poweroff        # sync + reboot(RB_POWER_OFF)
hzctl reboot                     # sync + reboot(RB_AUTOBOOT)
hzctl help

# Gentoo-style name-first (rc-service flavor):
hzctl <name> start
hzctl <name> stop
hzctl <name> restart
hzctl <name> status
hzctl <name> reload              # per-service SIGHUP
```

Socket path: `/run/hoshizora/control` (override with `HZ_CTL_PATH` env var). Permissions: `0660`, owned by root — only root can issue commands by default.

## What it does NOT do (yet)

Intentionally omitted — see `DEFERRED.md` for the full ledger. Add when actually needed:

- **io_uring** (uses `poll()` over six fds — already non-blocking)
- **transactional snapshot/restore** (CRIU's job if you really need process freeze/resume)
- **per-service capability drop** (PID 1 caps are dropped post-setup; per-service capsets deferred — add when a config asks)
- **non-root operator access** to the control socket (add a `hoshizora` group + chgrp when needed)
- **recursive fanotify walk** (the `recursive` keyword is parsed but only the top directory is marked — subdirectory changes won't fire)
- **`memory-high`, `io-weight`** cgroup fields (add when a config uses them)
- **bootloader / custom libc / slab allocator / .hsb bytecode** — all spec items dropped. Kernel + glibc static + text config is enough.
- **cron-syntax `0 3 * * *`** — date arithmetic, defer until a real config asks (interval-based `every:` covers most cases)
- **`isolate` target command** (stop everything not in a target's dep closure) — add when a real config needs systemd-style isolate semantics
- **OCI runtime compat / image format** for container integration — Hoshizora supervises processes, not images

## Sibling binaries

- **`hzctl`** — control client (~60 LOC). Connects to the socket, sends one line, prints the response.
- **`hzlog`** — syslog collector (~95 LOC). Binds `/dev/log` as `SOCK_DGRAM`, prepends receive-time, appends to `/var/log/messages`. Separate binary because PID 1 must not parse untrusted input.

## Build

```bash
make
```

Produces three statically-linked binaries:
- `./hoshizora` — the init system (~1.0 MB, static)
- `./hzctl` — the control client (~800 KB, static — glibc static is fat; `strip` or use musl for smaller binaries)
- `./hzlog` — the syslog collector (~960 KB, static)

## Self-check

```bash
make test
```

Runs `tests/testsuite.sh`, which executes six self-checks in sequence and tallies PASS/FAIL (currently 55 PASS / 0 FAIL):

- **`tests/core.sh`** against `tests/core.hs` (two `/bin/true` services with a dependency + one bad-exec service) — exercises every control-socket command in both verb orderings (action-first AND Gentoo name-first SOV), exercises `enable`/`disable`/`show`/`logs`/`daemon-reload`, asserts each step, and verifies the bad-exec service goes to FAILED without a respawn storm. Also verifies `reboot(2)` is invoked on shutdown (expected to fail with EPERM in non-PID-1 test env).
- **`tests/features.sh`** against `tests/features.hs` — verifies the parser accepts `memory-limit`, `cpu-weight`, `oom-kill`, `log`, `start-condition` (single + AND forms), `watch` blocks, `backoff(max=N)` extraction, `network_ready` virtual intent (verified at runtime against `/sys/class/net/`), AND that per-service log files are actually created on disk when services run.
- **`tests/onfail.sh`** against `tests/onfail.hs` — exercises the on-fail: runtime path end-to-end: a `/bin/false` service with `backoff(max=1)` and `on-fail: shutdown` crashes, the respawn timer (CLOCK_MONOTONIC) fires, start_service hits the backoff guard, `mark_failed()` calls `on_fail_trigger()` which logs `on-fail: shutdown` and sets `g_shutdown=1`, and hoshizora exits cleanly without external SIGTERM.
- **`tests/cron.sh`** against `tests/cron.hs` — verifies `every: "2s"` scheduling: a service that touches a marker file fires within 6s, re-arms after clean exit.
- **`tests/hzlog.sh`** — sends a syslog datagram via Python's `socket` module to an overridden `/dev/log`, asserts the line appears in the log file with receive-time prefix.
- **`tests/v2.sh`** against `tests/v2.hs` — v2.0 features: variable substitution (`$SOCK`), `timeout-start:`, socket activation (`listen:` Unix path), `target` block + `hzctl start <target>`, capability-dropping attempt (warns in sandbox), sd-notify socket bind.

A final `TESTSUITE SUMMARY` line prints total PASS / FAIL counts and the script exits non-zero if any check failed.

## Install

```bash
sudo make install   # copies hoshizora to /sbin/, hzctl + hzlog to /usr/bin/
```

Then either:
- `init=/sbin/hoshizora` on the kernel command line, or
- `ln -sf /sbin/hoshizora /sbin/init`

## Config format (subset of the original HCL-style grammar)

```hcl
# v2.0: top-level variable assignments — substituted into string fields below.
$WEB_ROOT = "/var/www"
$LOG_DIR  = "/var/log/hoshizora"

system "my-host" {
    service nginx {
        requires: [network_ready];              # virtual intent — satisfied when any non-lo iface is up
        exec: "/usr/sbin/nginx" with args ["-g", "daemon off;"];
        respawn: backoff(max = 5, base = 1s);   # max restarts (base currently ignored)
        memory-limit: 256MiB;                   # cgroup v2 memory.max (KiB/MiB/GiB/TiB)
        cpu-weight: 50;                         # cgroup v2 cpu.weight (1-10000, default 100)
        oom-kill: group;                        # cgroup v2 memory.oom.group=1
        log: "$LOG_DIR/nginx.log";              # per-service stdout+stderr redirect (O_APPEND)
        start-condition: file-exists("/etc/nginx/nginx.conf") and link-up("eth0");
        healthy: tcp-probe("127.0.0.1:80", 5s); # 3 consecutive fails → FAILED
        on-fail: restart(fallback_web);         # when nginx gives up, start the fallback
        every: "1h";                            # cron-style: fire hourly, re-arm on clean exit
        timeout-start: "30s";                   # v2.0: FAILED if not RUNNING in 30s
        expect-notify: true;                    # v2.0: service sends READY=1; timer disarms on it
        listen: "0.0.0.0:80";                   # v2.0: socket activation — Hoshizora binds, nginx inherits fd 3
        listen: "0.0.0.0:443";
        environment: {
            "NGINX_HOST": "localhost",
            "NGINX_PORT": "80"
        };
        # These fields are accepted but ignored — add when needed:
        # capabilities, transactional, snapshot
    }

    service webapp-container {
        exec: "/usr/bin/python3 app.py";
        namespace: private;                     # v2.0: unshare mount+net+pid
        rootfs: "/var/lib/hoshizora/roots/webapp";  # v2.0: pivot_root here
        bind: "/etc/resolv.conf:/etc/resolv.conf";  # v2.0: bind-mount src:dst
    }

    # File watches — on change, dispatch reload(name) or restart(name)
    watch "/etc/nginx/nginx.conf" {
        on-change: reload(nginx);
    }
    watch "/etc/postgresql/" recursive {
        on-change: restart(postgres);
    }

    # v2.0: target — named service group. `hzctl start multi-user` starts all three.
    target multi-user {
        requires: [nginx, webapp-container, postgres];
    }

    # These top-level constructs are accepted but ignored:
    # intents { ... }
    # starfield { ... }
}
```

## Architecture

Single-file C99 binary, single poll() loop over six fds:

```
                    ┌─────────────────────────────────────────┐
                    │           hoshizora (PID 1)             │
                    └─────────────────────────────────────────┘
                                    │
       ┌──────────┬────────────────┼───────────────┬──────────┬──────────┐
       ▼          ▼                ▼               ▼          ▼          ▼
   signalfd   control socket   timerfd          timerfd    fanotify   notify fd
   (SIGCHLD,  /run/hoshizora/  (next respawn    (5s health (file      /run/hoshizora/
   SIGTERM,   control          or cron fire)    probe +    watches;   notify
   SIGINT,                                     start_     -1 if      (sd_notify;
   SIGHUP)                                     deadline   none)      -1 if none)
       │          │                │               │          │          │
       ▼          ▼                ▼               ▼          ▼          ▼
   reap_        handle_         fire_due_       run_health_ handle_   handle_notify_
   children()   command()       respawns()      checks()   fanotify_ event()
   reload_                      start_service() tcp_probe  event()
   config()                                     +start_
   shutdown_                                    deadline
   all()                                        check
```

No threads, no async, no event loop library. One process, one loop, six fds.

**Capability dropping**: after `setup_*` calls complete, `capset(2)` keeps only `CAP_SYS_ADMIN` (cgroups, pivot_root, mount), `CAP_KILL` (signaling services), `CAP_SYS_BOOT` (reboot), `CAP_NET_ADMIN` (binding privileged ports for socket activation). All other capabilities dropped. Warns and continues with full caps if `capset` fails (e.g. sandbox without `CAP_SETPCAP`).

**Parallel shutdown**: on SIGTERM/SIGINT, all running services get SIGTERM simultaneously (reverse order — dependents first), then a single 5s `waitpid` poll reaps them all. Stragglers get SIGKILL in one pass. Total shutdown time is O(5s), not O(N×5s). Reload uses the same parallel-stop pattern for changed services.

**Poweroff / reboot**: after shutdown_all, PID 1 calls `sync()` + `reboot(RB_POWER_OFF)` (or `RB_AUTOBOOT` for `hzctl reboot`). Returning from `main()` would panic the kernel ("Attempted to kill init!").

**Socket activation**: at startup, `setup_listen_sockets()` binds all configured `listen:` addresses. When `start_service` runs, the child's listen fds are dup'd to fd 3+ and `HZ_LISTEN_FDS=N` is set in the child env. Services can `accept()` directly without needing to bind.

**sd-notify**: services with `expect-notify: true;` get `HZ_NOTIFY_SOCKET=/run/hoshizora/notify` in their env. They send `<service-name> READY=1` datagrams to that socket. Hoshizora's `handle_notify_event` parses them, disarms `start_deadline` on READY, logs transitions.

**Container integration**: in the child post-fork, `setup_container_child()` runs `unshare(CLONE_NEWNS|CLONE_NEWNET|NEWPID)` if `namespace: private;` is set, brings up loopback in the new netns, then `pivot_root`s into `rootfs:` if set, then bind-mounts each `bind:` entry. All degrade to non-fatal warnings if capabilities are missing.

## Memory / CPU targets

- RSS: < 1 MiB (static binary, single process, no mmap'd arenas)
- CPU idle: 0% (uses `poll()` with timeout = next pending respawn)

## License

MIT — see `LICENSE`.
