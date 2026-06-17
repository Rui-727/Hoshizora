# Hoshizora Architecture

A single-file PID 1 init system for Linux/x86_64. About 2,382 lines of C99 in
`init.c`, ~60 lines in `hzctl.c`, ~97 lines in `hzlog.c`, one header (157
lines). Statically linked, libc-only, no dependencies beyond the kernel
(`capset(2)` invoked via raw syscall — no libcap).

Hoshizora is built to be small enough to read in an afternoon, explicit about
what it does and does not do, and free of framework jargon. Every feature in
this document exists in code; deliberate omissions are tagged inline in the
source with `deferred:` comments and tracked in `DEFERRED.md`.

## Design principles

- **Modular** — features that can be compiled out or disabled at runtime.
  The core (`init.c`) is a single poll() loop; cgroups, fanotify, health
  probes, cron scheduling, socket activation, sd-notify, and container
  namespaces are independent code paths that no-op cleanly when their
  kernel feature is missing.
- **Transparent** — readable code, predictable behavior. No threads, no
  async runtime, no event-loop library. One process, one loop, six fds.
  Every runtime decision is logged with the service name and reason.
- **Minimal by default** — the core ships only what a real config needs.
  Optional features are tracked in `DEFERRED.md` and added when a concrete
  use case arrives, not speculatively.

See `ROADMAP.md` for the v2.0 feature set (shipped) and forward-looking
items still on the deferral list.

---

## 1. Component map

```
                       +-----------------------------+
                       |          init.c (PID 1)     |
                       |                             |
   /etc/hoshizora/     |  config parser  ───────────┼──►  g_sys.services[]
   system.hs  ───────► |                             |     g_sys.watches[]
                       |  service state machine      |
                       |  fork+exec / cgroup / fan   |
                       |                             |
   /run/hoshizora/     |  control socket  ◄──────────┼── hzctl.c
   control  ◄──────────┤                             |
                       |  poll() event loop          |
                       |   ├ signalfd                |
                       |   ├ control socket          |
                       |   ├ timerfd (respawn)       |
                       |   ├ timerfd (health 5s)     |
                       |   └ fanotify fd             |
                       +-----------------------------+
                                    │
                                    ▼
                            reboot(RB_POWER_OFF |
                                    RB_AUTOBOOT)
```

| File                  | Role                                                | LOC  |
|-----------------------|-----------------------------------------------------|------|
| `init.c`              | PID 1: parser, state machine, event loop, control   | 1821 |
| `hzctl.c`             | Control client: argv → Unix socket → stdout         | 64   |
| `include/hoshizora.h` | Public structs + limits                             | 116  |
| `system.hs`           | Example root config (postgres + nginx + conditions) | 62   |
| `Makefile`            | `make` → `./hoshizora` + `./hzctl`; `make test`     | 35   |
| `tests/`              | Self-checks + their `.hs` configs                   | 9 files, 40 assertions |

Static limits (in `hoshizora.h`):

| Constant          | Value | What it bounds                       |
|-------------------|-------|--------------------------------------|
| `HZ_MAX_SERVICES` | 64    | services per system                  |
| `HZ_MAX_WATCHES`  | 32    | fanotify watch entries               |
| `HZ_MAX_ARGS`     | 32    | argv slots per service               |
| `HZ_MAX_DEPS`     | 8     | `requires:` entries per service      |
| `HZ_MAX_ENV`      | 32    | `environment {}` key/val pairs       |
| `HZ_MAX_NAME`     | 64    | service name length                  |
| `HZ_MAX_PATH`     | 256   | exec path / log path / start-cond arg |
| `HZ_MAX_STR`      | 256   | generic string token / env value     |

Sized for a real-world host init (a typical box runs 50–150 supervised units).
Bumped when a real config hits the ceiling, never speculatively.

---

## 2. Process model

Hoshizora **is** PID 1. There is no parent supervisor, no respawn-by-init
trick. Implications:

- `main()` must never return — kernel panics with "Attempted to kill init!".
  Shutdown therefore ends with `reboot(RB_POWER_OFF)` (or `RB_AUTOBOOT`),
  with `reboot(RB_HALT_SYSTEM)` as the fallback if `reboot()` itself fails
  (e.g. `CAP_SYS_BOOT` dropped).
- All signals are handled via `signalfd(2)`, not signal handlers — handlers
  in PID 1 are a footgun (async-signal-safety constrains everything).
- Children are reaped in the main loop, not via a `SIGCHLD` handler.
- The event loop is single-threaded; no `fork()`-off-the-main-thread.

Children are forked from PID 1 directly (no intermediate supervisor process).
Each child:

1. Calls `setsid()` to detach from any inherited controlling terminal.
2. Resets the signal mask to empty — the parent blocks SIGCHLD/SIGTERM/SIGINT/
   SIGHUP for `signalfd`; without an explicit unblock, `kill <service-pid>`
   would be deferred until the service unblocked signals itself.
3. Has stdout+stderr redirected to `log:` path (if set) via `dup2` in fork,
   with `O_APPEND` so restarts don't clobber history.
4. Inherits PID 1's environment, with `environment {}` block entries
   appended (last write wins).
5. Is `execve()`'d with the configured `exec:` + args.
6. On `execve` failure the child writes one log line and `_exit(127)` — the
   parent treats exit 127 as a non-respawnable config error.

If the service has a cgroup spec, `cgroup_setup_for()` runs **before** the
fork (mkdir + write `memory.max` / `cpu.weight` / `memory.oom.group`), and
`cgroup_assign_pid()` runs **after** the fork in the parent (write child pid
to `cgroup.procs`).

---

## 3. Event loop

Single-threaded `poll(2)` over **at most 5 fds**:

| # | fd           | Source                | Triggers                          |
|---|--------------|-----------------------|-----------------------------------|
| 0 | `g_sigfd`    | `signalfd(2)`         | SIGCHLD, SIGTERM, SIGINT, SIGHUP  |
| 1 | `g_ctlfd`    | `AF_UNIX` listener    | new `hzctl` client connection     |
| 2 | `g_reloadfd` | `timerfd_create(2)`   | next pending respawn fires        |
| 3 | `g_healthfd` | `timerfd_create(2)`   | 5s tick for `healthy:` probes     |
| 4 | `g_fanfd`    | `fanotify_init(2)`    | `watch:` path changed             |

If fanotify isn't available (no `CAP_SYS_ADMIN` or no watches configured),
`g_fanfd` is `-1` and the loop drops to 4 fds. If the health timer fails to
allocate, `g_healthfd` is `-1` and the health tick is silently skipped
(health probes are best-effort).

The loop:

```c
while (!g_shutdown) {
    arm_respawn_timer();          /* re-arm fd 2 for next pending respawn */
    int r = poll(pfds, npfds, -1);
    if (r < 0 && errno == EINTR) continue;

    if (pfds[0].revents & POLLIN) handle_signal();         /* SIGCHLD/SIGTERM/SIGINT/SIGHUP */
    if (pfds[1].revents & POLLIN) {                        /* hzctl client */
        int cfd = accept4(g_ctlfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd >= 0) handle_control_client(cfd);
    }
    if (pfds[2].revents & POLLIN) fire_due_respawns();     /* timerfd tick */
    if (g_healthfd >= 0 && pfds[3].revents & POLLIN)
        run_health_checks();                                /* 5s tick */
    if (npfds > 4 && pfds[4].revents & POLLIN)
        handle_fanotify_event();                            /* file watch */
}

LOGI("shutdown signal received");
shutdown_all();                /* parallel stop of all running services */
reap_children();               /* final waitpid sweep */
unlink(g_ctl_path);
sync();
if (g_reboot_target) reboot(RB_AUTOBOOT);
else                 reboot(RB_POWER_OFF);
/* fallback if reboot() failed: */
reboot(RB_HALT_SYSTEM);
```

`poll()` is used instead of `io_uring` because the fd count is fixed at ≤5;
the syscall overhead is microseconds, and skipping io_uring removes a whole
subsystem from the binary.

---

## 4. Config grammar (`system.hs`)

Token-stream walker, no separate lexer pass, no AST. The parser consumes
tokens directly into `hz_service_t` / `hz_watch_t` structs.

### Top level

```
system "name" {
    service NAME { ... }
    service NAME { ... }
    watch "path" { ... }
    ...
}
```

Anything that isn't `service IDENT {` or `watch STRING [recursive] {` is
skipped naturally — its braces and strings get filtered as non-IDENT tokens,
its closing `}` is consumed by the punctuation case. No depth tracking
needed for `intents { ... }` / `starfield { ... }` / similar block-level
constructs that the spec mentions but the runtime doesn't honor.

### Service block

```
service nginx {
    exec: "/usr/sbin/nginx" with args ["-g", "daemon off;"];
    requires: [ "network_ready", "postgres" ];   # built-in intents + real services
    respawn: backoff(max = 5, base = 1s);        # max=N honored; base= ignored
    memory-limit: 256MiB;                        # cgroup v2 memory.max
    cpu-weight: 50;                              # cgroup v2 cpu.weight (1-10000)
    oom-kill: group;                             # cgroup v2 memory.oom.group=1
    log: "/var/log/hoshizora/nginx.log";         # per-service stdout+stderr redirect
    start-condition: file-exists("/etc/nginx/nginx.conf")
                     and link-up("eth0");        # max 2 builtins, AND only
    healthy: tcp-probe("127.0.0.1:80", 5s);      # 3 consecutive fails → FAILED
    on-fail: restart(fallback_web);              # or: shutdown;
    environment: {
        "NGINX_HOST": "localhost",
        "NGINX_PORT": "80"
    };
}
```

### Watch block

```
watch "/etc/nginx/nginx.conf" {
    on-change: reload(nginx);      # SIGHUP the service
}
watch "/etc/postgresql/" recursive {
    on-change: restart(postgres);  # stop + start
}
```

### What the parser deliberately ignores

These tokens are accepted for forward-compat but not honored. Each carries a
`deferred:` comment in `parse_service_block` / `parse_watch_block` and is
listed in `DEFERRED.md`:

- `recursive` keyword in `watch` — fanotify marks the top directory only.
  Add `FAN_MARK_FILESYSTEM` (kernel ≥ 5.16) when a real config needs subtree
  watches.
- `backoff(base = Xs)` — linear backoff = `restart_count` seconds, capped at
  30. `base` is redundant.
- `capabilities`, `user`, `group`, `no-new-privileges`, `transactional`,
  `snapshot` — services inherit PID 1's environment; per-service capability
  dropping is YAGNI until a config actually requests it.

Unknown fields inside a service block are skipped via `skip_unknown_field()`,
which tracks `{}`/`[]`/`()` nesting so a `[ ... ]` list value doesn't end
the field early.

### Size suffixes

`parse_size()` accepts `KiB`, `MiB`, `GiB`, `TiB` (binary, IEC) and a bare
number (bytes). Decimal `K`/`KB`/`MB`/`GB` were dropped because no real config
used them — the README documents IEC only, and cgroup v2 `memory.max` wants
bytes.

---

## 5. Service state machine

Three states only.

```
                  start_service()
       ┌─────────────────────────────────┐
       │                                 ▼
   STOPPED ◄────── stop_service() ──── RUNNING
       │                                 │
       │                                 │ exec fails (exit 127)
       │                                 │ ─ or ─
       │                                 │ restart_count >= max_restarts
       │                                 │ ─ or ─
       │                                 │ fork fails
       │                                 │ ─ or ─
       │                                 │ health probe 3× fail
       │                                 ▼
       └───────────────────────────── FAILED
                                         │
                                         │ on_fail_trigger():
                                         │   - RESTART(other): stop+start target
                                         │   - SHUTDOWN: g_shutdown = 1
                                         ▼
                                    (no exit from FAILED
                                     except reload or manual restart)
```

### `start_service()` — ~110 lines, the largest single function

1. Skip if already `RUNNING`.
2. Clear `manual_stop` and `respawn_at`.
3. Skip if disabled — i.e. if a marker file exists at
   `<ctl-dir>/enabled/<name>`. Ephemeral state: survives reload, not reboot.
4. Evaluate `start-condition:` — if false, log "skipping" and return 0.
   Service stays `STOPPED`. Re-evaluated on each `start` call so the
   operator can retry once the condition becomes true.
5. Walk `requires:` — every name must be either a real service in
   `RUNNING` state (recurse to start it if not), or the virtual intent
   `network_ready` / `network-ready` (satisfied when any non-`lo` interface
   has `IFF_UP` set in `/sys/class/net/<dev>/flags`). Unknown dep names log
   a warning and are tolerated (forward-reference compat).
6. Backoff guard: if `restart_count >= max_restarts` and `max_restarts > 0`,
   call `mark_failed()` and return.
7. `cgroup_setup_for()` — mkdir + write limits (no-op if cgroup v2 missing
   or no limits requested).
8. `fork()`. In child: `setsid()`, unblock signals, optional log redirect,
   build argv + envp, `execve()`. On `execve` failure: log + `_exit(127)`.
9. In parent: record pid, set `state=RUNNING`, `cgroup_assign_pid()`.

### `stop_service()` — ~30 lines

1. If not running, still set `manual_stop=1` so a pending respawn cancels.
2. SIGTERM the pid.
3. Poll `waitpid(WNOHANG)` up to 50 × 100 ms (5 s total).
4. If still alive, SIGKILL + blocking `waitpid`.
5. Set `state=STOPPED`, clear pid, `cgroup_teardown_for()`.

`cgroup_teardown_for()` does `rmdir(path)` on the per-service cgroup
directory. The kernel won't allow `rmdir` on a cgroup with live processes,
so by the time we reach teardown the SIGKILL above has already reaped the
last child. We don't write `cgroup.kill` — `rmdir` is sufficient and avoids
a kernel-version dependency.

### `reap_children()` — called every loop iteration

When a child dies:

1. `waitpid(-1, WNOHANG)` sweep until no more children.
2. For each dead pid, find the matching service by `s->pid`.
3. If we're shutting down: state→`STOPPED`, break.
4. If exit status is 127 (`execve` failed): mark `FAILED` immediately,
   do **not** respawn (would be an instant respawn storm).
5. Else if crashed (non-zero exit or signal) and `respawn` is set and
   `manual_stop` is clear:
   - `restart_count++`
   - state→`STOPPED` (so `start_service` will accept the respawn — the
     short-circuit `if (state == RUNNING) return 0` would otherwise block
     respawn forever).
   - `respawn_at = mono_now() + delay` where `delay = min(restart_count, 30)`
     seconds. The respawn timer fires `start_service()` when due.
6. Else if clean exit (WIFEXITED, status 0): state→`STOPPED`,
   **`restart_count = 0`** — a successful run earns a fresh backoff budget.
7. Else (crashed without `respawn`): `mark_failed()`.

### Backoff

Linear: each respawn waits `restart_count` seconds, capped at 30. No
exponential, no jitter. When `restart_count` reaches `max_restarts` (from
`backoff(max=N)`, default 5), the next `start_service()` call goes to
`FAILED` and `on_fail_trigger()` fires.

### `mark_failed()` + `on_fail_trigger()`

`mark_failed(s)` is the single FAILED-transition site — it sets `state =
HZ_S_FAILED` and calls `on_fail_trigger(s)`. All four runtime paths that
should land a service in FAILED go through it: exec-fail (exit 127),
backoff exhausted, fork failure, crash without `respawn`.

`on_fail_trigger()` dispatches based on `s->on_fail.kind`:

- `HZ_ON_FAIL_NONE` — no-op.
- `HZ_ON_FAIL_SHUTDOWN` — set `g_shutdown = 1`; main loop will pick it up
  on the next iteration and call `shutdown_all()`.
- `HZ_ON_FAIL_RESTART` — look up the target service; if it's currently
  `RUNNING`, `stop_service()` it first, then `start_service()` it.

No cycle guard. The operator wrote the config; cycles are their bug to
spot in the logs.

---

## 6. Control socket (`hzctl` protocol)

`AF_UNIX`, `SOCK_STREAM`, path `/run/hoshizora/control` (override via
`HZ_CTL_PATH` env). One connection per command — no multiplexing. The
parent directory is created with `mkdir(0755)` if missing. The socket file
is `unlink()`'d before `bind()` (in case a previous PID 1 crashed without
cleaning up) and at shutdown.

### Wire format

```
client → server:   "<verb> [args...]\n"   (single line, then SHUT_WR)
server → client:   "<response>\n"          (or multi-line, then close)
```

`hzctl.c` is a 64-line client: `socket() → connect() → write() → shutdown(SHUT_WR) → read loop`.

### Commands

Both verb orderings work for service-specific commands — action-first
(systemd-ish) and name-first Gentoo-style (`rc-service` flavor). The
reorder logic is in `handle_control_client()` at the top: if `argv[1]`
isn't a recognized top-level verb and `argv[2]` is, swap them.

| Command                              | Action                                                    |
|--------------------------------------|-----------------------------------------------------------|
| `list`                               | Print all services with state/pid/restarts/disabled/cond  |
| `status` / `status <name>`           | All services or one                                       |
| `start <name>` / `<name> start`      | Start service (if STOPPED, not disabled, condition true)  |
| `stop <name>`  / `<name> stop`       | Stop service (SIGTERM → 5s → SIGKILL)                     |
| `restart <name>` / `<name> restart`  | Stop + start                                              |
| `reload` / `daemon-reload`           | Re-read config, diff + apply (see §8)                     |
| `reload <name>` / `<name> reload`    | Per-service SIGHUP                                        |
| `enable <name>` / `disable <name>`   | Ephemeral: create/remove `<ctl-dir>/enabled/<name>`       |
| `show`                               | Print enable/disable state for all services               |
| `logs [N]`                           | Dump last N lines from the 8 KiB ring buffer (default 50) |
| `shutdown` / `poweroff`              | poweroff (`g_reboot_target=0; g_shutdown=1`)              |
| `reboot`                             | reboot   (`g_reboot_target=1; g_shutdown=1`)              |
| `help` / `?`                         | Usage                                                     |

### Log ring

8 KiB byte-addressed ring (`g_log_ring[]` + `g_log_ring_pos` +
`g_log_ring_used`). Every `log_msg()` writes to both stderr and the ring.
`logs_dump()` walks backward counting newlines to find the start of the
Nth-from-last line, then writes the slice (handling wrap in up to two
`write()` calls). 8 KiB is enough for ~50–100 typical log lines, fits in
a single page, no allocation, no growth policy.

### Enable/disable

A file at `<ctl-dir>/enabled/<name>` means "disabled". File absent = enabled.
`disable X` mkdir-p's the `enabled/` subdir and creates the marker; `enable X`
unlinks it. Tied to the control socket's parent dir so it works in test mode
(`HZ_CTL_PATH` override) too. State survives reload, not reboot. For
persistent enable/disable, edit the config.

---

## 7. Cgroups v2

Detected at startup via `statfs("/sys/fs/cgroup")` and compared against
`HZ_CGROUP2_SUPER_MAGIC` (`0x63677270`, hard-coded to avoid the
`<linux/magic.h>` include).

Per-service directory: `/sys/fs/cgroup/hoshizora/<service_name>/`. Base
dir `/sys/fs/cgroup/hoshizora/` is `mkdir()`'d once per start (EEXIST is
fine).

| File written               | When                                |
|----------------------------|-------------------------------------|
| `memory.max`               | if `memory-limit:` set (bytes)      |
| `cpu.weight`               | if `cpu-weight:` set (1–10000)      |
| `memory.oom.group`         | if `oom-kill: group;` set (writes 1)|
| `cgroup.procs`             | after fork, with child's pid        |

Teardown on stop: `rmdir()` the per-service directory. The kernel refuses
to remove a cgroup with live processes, so teardown only runs after
`waitpid()` has confirmed the child is dead. We don't write `cgroup.kill`
— `rmdir` after SIGKILL is sufficient and works on older kernels too.

If cgroup v2 isn't mounted (typical in containers), the parser still
accepts the config, the runtime skips cgroup setup silently, and a
one-time warning is logged at startup if any service actually requested
limits. Without that one-time check, limits would silently not apply.

---

## 8. Reload (live re-apply)

Triggered by SIGHUP, `hzctl reload` (no arg), or `hzctl daemon-reload`.
`reload_config()` is ~80 lines, diff-and-apply:

1. Snapshot the current `g_sys.services[]` (full structs) into `old[]`.
2. Reset `g_sys.n_services = 0` and re-parse the config file into `g_sys`.
   If parse fails, restore `old[]` and abort — old services keep running
   untouched.
3. Walk new config:
   - For each new service, look it up by name in `old[]`:
     - If `service_spec_changed()` returns true → mark for parallel stop,
       preserve old pid/state, set `manual_stop=1` so stop won't try to
       respawn.
     - Else → adopt the old pid/state/restart_count/manual_stop; the
       service keeps running unchanged.
4. `stop_parallel()` all changed services in one 5 s window.
5. Clear `manual_stop` on the stopped services so the start pass can
   pick them up.
6. Walk `old[]`: any service not present in new config → SIGTERM its pid
   (it's been removed from the config).
7. Walk new config: any service in `STOPPED` state that isn't
   `manual_stop` → `start_service()`.

`service_spec_changed()` compares: `exec`, `argv`, `env`, `n_deps`,
`deps`, `respawn`, `memory_limit`, `cpu_weight`, `oom_kill_group`,
`log_path`, `on_fail.kind` + `on_fail.target`.

It deliberately does **not** compare:

- `start-condition` / `healthy` — re-evaluated at every `start` call and
  every 5 s health tick respectively, so a config change takes effect
  without a restart.
- `max_restarts` — start-time effect (backoff guard). Restart picks up the
  new value.
- `restart_count` / `pid` / runtime fields — preserved across reload for
  unchanged services.

No transactional snapshot. If the parse fails mid-way, the old `g_sys`
stays intact and an error is logged. A real init rarely needs atomic
reload — if a config is broken, the operator fixes it and reloads again.

---

## 9. Health probes

`g_healthfd` is a `timerfd_create(CLOCK_MONOTONIC)` set to 5 s intervals.
On each tick, `run_health_checks()` iterates every service with
`healthy: tcp-probe(...)` set and state `RUNNING`:

1. `tcp_probe(hostport, timeout_s)` — `socket()`, `setsockopt(SO_SNDTIMEO)`,
   `connect()`. Returns 0 on success, -1 on failure/timeout. No data sent
   or received — just checks the listener is alive.
2. Success → reset `fail_count` to 0.
3. Failure → `fail_count++`. After **3** consecutive failures, log
   "unhealthy — marking FAILED", SIGTERM the service, reset `fail_count`.
   `reap_children()` will see the exit; respawn logic kicks in if
   `respawn:` is set. `manual_stop` is left at 0 so respawn isn't blocked.

Only `tcp-probe` is supported — it covers HTTP servers, postgres, redis,
etc. (none of the real configs need anything else). `http-probe`,
`exec-probe`, `unix-socket-probe` are deliberately omitted.

Probes are sequential (max 64 services × 5 s timeout = 320 s worst case),
but real services answer in <10 ms and timeouts are rare. Acceptable for
PID 1.

---

## 10. Watch (fanotify)

`setup_fanotify()` creates one `fanotify_init(FAN_CLASS_NOTIF | FAN_NONBLOCK,
O_RDONLY)` fd and adds a `FAN_MARK_ADD` for each `watch:` path with mask
`FAN_MODIFY | FAN_CLOSE_WRITE | FAN_MOVED_FROM | FAN_MOVED_TO | FAN_CREATE
| FAN_DELETE`.

On event (`handle_fanotify_event()`, ~40 lines):

1. `read()` fixed-size `fanotify_event_metadata` records from the fd.
2. For each event: `readlink("/proc/self/fd/<event-fd>")` to resolve the
   changed path, close the event fd.
3. Longest-prefix-match the path against `g_sys.watches[]` — pick the
   watch whose `path` is the longest prefix of the event path.
4. Look up the target service.
5. If `action == HZ_W_RELOAD`: `kill(service->pid, SIGHUP)` — let the
   service reload itself.
6. If `action == HZ_W_RESTART`: `stop_service()` + `start_service()`.

Single-level marks only — `recursive` is accepted by the parser but
currently ignored. If a real config needs subtree watches, add
`FAN_MARK_FILESYSTEM` (kernel ≥ 5.16) — it's a one-line change.

If `fanotify_init` fails (no `CAP_SYS_ADMIN` — typical in test mode), the
fd stays `-1`, a warning is logged, and watches become inert. Services
still run, just no auto-reload/restart on file changes.

---

## 11. Signal handling

`setup_signalfd()` blocks SIGCHLD, SIGTERM, SIGINT, SIGHUP on the main
thread and creates a `signalfd` that the poll loop watches. SIGPIPE is
set to `SIG_IGN` — services that write to closed pipes should crash and
respawn rather than take PID 1 with them.

| Signal   | Action                                                       |
|----------|--------------------------------------------------------------|
| SIGCHLD  | `reap_children()` — sweep waitpid, update service state      |
| SIGTERM  | poweroff (`g_shutdown = 1`, `g_reboot_target` unchanged = 0) |
| SIGINT   | poweroff (same path as SIGTERM — same handler case)          |
| SIGHUP   | `reload_config(g_cfg_path)`                                  |

SIGINT and SIGTERM both set `g_shutdown = 1`. They do **not** differ in
reboot target — that's controlled by `hzctl shutdown` vs `hzctl reboot`,
which set `g_reboot_target` explicitly before flipping `g_shutdown`. If
you want SIGINT to mean "reboot" instead of "poweroff", add a separate
case in `handle_signal()`.

All other signals keep default disposition — but PID 1's default for
most is to ignore, so this is fine.

---

## 12. What was deliberately not built

Each item is documented in code with a `deferred:` comment or in
`DEFERRED.md`:

- **`io_uring` event loop** — `poll()` over 6 fds is microseconds.
- **Transactional config snapshot / rollback** — broken configs get fixed
  and reloaded; we don't need a journal.
- **Per-service capability drop** — PID 1 caps are dropped post-setup
  (see §11); per-service capsets deferred. The parser accepts and skips
  the `capabilities:` field. Add when a real config asks.
- **Non-root operator access** to `hzctl` — the control socket is `0660`
  root:root. Operators are root. Add a polkit-style gate if a real
  deployment needs it.
- **`recursive` fanotify marks** — see §10. Single-level covers the
  current `watch:` paths.
- **Decimal size suffixes** (`K`/`MB`) — every real config uses IEC.
- **`backoff(base=Xs, type=exponential)`** — linear backoff with a 30 s
  cap is what every real service actually wants.
- **`http-probe`, `exec-probe`, `unix-socket-probe`** — `tcp-probe`
  covers HTTP servers (just probe the port), postgres, redis, etc.
- **DBus / sd-bus compatibility** — different design space; if a service
  needs DBus activation, run a separate dbus broker as a service.
- **`systemctl`-compatible CLI** — `hzctl` is intentionally smaller.
  A wrapper that translates `systemctl start X` → `hzctl X start` is
  ~20 lines if anyone needs it.
- **Cron-syntax `0 3 * * *`** — date arithmetic; interval-based `every:`
  covers most cases. Add when a real config needs calendar scheduling.
- **`isolate` target command** — stop everything not in a target's dep
  closure. Add when a real config needs systemd-style isolate semantics.
- **`start-after:`** — `requires:` already implies ordering. Add when
  decoupled hint-vs-dep is needed.
- **OCI runtime compat / image format** for container integration —
  Hoshizora supervises processes via `unshare` + `pivot_root`, not images.
- **systemd-style `LISTEN_FDS` env var** — we use `HZ_LISTEN_FDS` to
  avoid colliding with services expecting exact systemd semantics.

---

## 13. Test surface

`make test` runs `tests/testsuite.sh`, which executes six self-checks:

| Script              | Config            | Asserts                                                |
|---------------------|-------------------|--------------------------------------------------------|
| `tests/core.sh`     | `tests/core.hs`   | parse, fork+exec, deps, exec-fail, all CLI verbs, both verb orderings, enable/disable/show/logs, daemon-reload, reboot(2) invoked |
| `tests/features.sh` | `tests/features.hs` | memory-limit, cpu-weight, oom-kill, log, start-condition (single + AND), watch, backoff(max=N), network_ready virtual intent, per-service log files created |
| `tests/onfail.sh`   | `tests/onfail.hs` | on-fail: parser, respawn timer fires, backoff guard, on_fail_trigger fires `shutdown`, clean exit without external SIGTERM |
| `tests/cron.sh`     | `tests/cron.hs`   | every: parser, cron_next scheduling, fire, re-arm after clean exit, marker file appears |
| `tests/hzlog.sh`    | (no .hs)          | hzlog binds /dev/log, datagram received, line appears in log file with timestamp |
| `tests/v2.sh`       | `tests/v2.hs`     | v2.0: $var substitution, timeout-start, listen (socket activation), target block + hzctl start <target>, capset attempted, notify socket bound |

55 assertions total, all green. Each self-check starts `./hoshizora` (or
`./hzlog`) with its `.hs` config and a temp `HZ_CTL_PATH` /
`HZ_NOTIFY_PATH`, exercises the control socket via `./hzctl`, and greps
the log file for expected lines.

What the tests do **not** cover (would require root / PID 1 / real hardware):

- Actual cgroup enforcement (only the parser + warning path)
- fanotify event firing (only setup without crashing)
- `reboot(RB_POWER_OFF)` actually powering off (test env gets EPERM,
  which the test asserts as proof the syscall was invoked)
- Health probe marking FAILED (needs a real listener to fail against —
  covered indirectly by `onfail.sh` exercising the same `mark_failed()`
  path)
- Actual `capset` dropping caps (sandbox lacks `CAP_SETPCAP` — test asserts
  the warning log line as proof the call was attempted)
- Actual `pivot_root` / `unshare` (needs root + real rootfs — test asserts
  parser accepts the fields, runtime path is exercised but degrades to
  a warning in sandbox)
- Actual sd_notify READY=1 round-trip (needs a real service that speaks
  the protocol — test asserts the socket is bound and the parser is wired)

---

## 14. Build & install

```bash
make            # → ./hoshizora (static, ~950 KiB) + ./hzctl (static, ~800 KiB)
make test       # → 40 assertions, ~5 s wall clock
sudo make install
                # → /sbin/hoshizora, /usr/bin/hzctl
```

To use as PID 1, either:

- `init=/sbin/hoshizora` on the kernel cmdline, **or**
- `ln -sf /sbin/hoshizora /sbin/init` (overrides the systemd symlink)

Config is read from `/etc/hoshizora/system.hs` by default, or the path
passed as `argv[1]` (used by the tests). If neither exists, `./system.hs`
in the current directory is tried as a last resort.

---

## 15. File map

```
Hoshizora/
├── .gitignore
├── LICENSE
├── Makefile
├── README.md              ← user-facing intro + quickstart
├── ARCHITECTURE.md        ← this file
├── ROADMAP.md             ← v1.0/v1.1 history + future candidates
├── init.c                 ← PID 1 (1,821 LOC)
├── hzctl.c                ← control client (64 LOC)
├── include/
│   └── hoshizora.h        ← public structs + limits (116 LOC)
├── system.hs              ← example real-world root config
└── tests/
    ├── testsuite.sh       ← entry point for `make test`
    ├── core.sh            ┐
    ├── core.hs            ┤  paired script + config
    ├── features.sh        ┤
    ├── features.hs        ┤
    ├── onfail.sh          ┤
    └── onfail.hs          ┘
```

Source: ~2,001 LOC of C. Tests: 9 files, 40 assertions. Docs: 3 files.
No external dependencies. No build system beyond `make`. No runtime
dependencies beyond libc and the Linux kernel.
