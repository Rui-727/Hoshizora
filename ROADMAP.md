# Hoshizora Roadmap

Ship what's actually needed. No new pillars, no new backends, no new file
formats, no new build tools, no new subsystems — until a real config asks
for them.

## Design principles

- **Modular** — features that can be compiled out or disabled at runtime.
  The core is a single poll() loop; cgroups, fanotify, health probes,
  cron scheduling, socket activation, sd-notify, and container namespaces
  are independent code paths that no-op cleanly when their kernel feature
  is missing.
- **Transparent** — readable code, predictable behavior. No threads, no
  async runtime, no event-loop library. One process, one loop, six fds.
  Every runtime decision is logged with the service name and reason.
- **Minimal by default** — the core ships only what a real config needs.
  Optional features are tracked in `DEFERRED.md` and added when a concrete
  use case arrives, not speculatively.

---

## v1.x — what's shipped

| Capability | Status | Notes |
|---|---|---|
| Text config parser | ✅ | HCL subset, `system.hs` |
| Service lifecycle | ✅ | fork+exec, dep-ordered parallel start |
| Zombie reaping | ✅ | PID 1's defining job |
| SIGTERM/SIGINT clean shutdown | ✅ | parallel stop, O(5s) |
| Crash respawn with backoff | ✅ | linear, capped at 30s, max=N from config |
| execve failure detection | ✅ | exit 127 → FAILED, no respawn storm |
| Signal mask hygiene in children | ✅ | parent blocks for signalfd, children get clean mask |
| Control socket + `hzctl` | ✅ | text protocol, both verb orders (action-first & Gentoo name-first) |
| cgroup v2 per-service | ✅ | `memory.max`, `cpu.weight`, `memory.oom.group` |
| fanotify file watches | ✅ | `reload(X)` / `restart(X)` on change |
| `start-condition:` | ✅ | 3 builtins (`file-exists`, `link-up`, `fs-mounted`), AND-of-2 |
| `healthy:` probe | ✅ | `tcp-probe("host:port", Ns)`, 3 fails → FAILED |
| `backoff(max=N)` | ✅ | parser extracts N into `max_restarts` |
| `reboot(2)` on shutdown | ✅ | poweroff / reboot, no kernel panic |
| `enable` / `disable` | ✅ | ephemeral marker file in `<ctl-dir>/enabled/` |
| `logs [N]` | ✅ | in-memory ring, 8 KiB |
| `reload <name>` | ✅ | per-service SIGHUP, distinct from daemon-reload |
| `reboot` cmd | ✅ | vs `shutdown` = poweroff |
| Per-service log redirect | ✅ | `log: "/path"` (O_APPEND, survives restart) |
| `network_ready` virtual intent | ✅ | satisfied when any non-`lo` iface has IFF_UP |
| `on-fail:` action | ✅ | `restart(other)` or `shutdown`, fires on FAILED transition |
| `oom-kill: group` | ✅ | writes `memory.oom.group=1` |
| `every:` cron scheduling | ✅ | interval-based, re-arms on clean exit (v1.2) |
| `hzlog` syslog collector | ✅ | separate binary, ~95 LOC, binds `/dev/log` (v1.2) |

---

## v2.0 — shipped

All nine features from the v2.0 plan are implemented and covered by
self-checks. Total v2.0 additions: ~510 LOC in `init.c`, ~36 LOC in
`include/hoshizora.h`, 1 new test pair (`tests/v2.sh` + `tests/v2.hs`).
External deps: still libc only — `capset(2)` is invoked via raw syscall,
no libcap.

### Theme 1 — Security & robustness

#### 1.1 Capability dropping  ✅ shipped

PID 1 calls `capset(2)` after `setup_*` completes. Keeps `CAP_SYS_ADMIN`
(cgroups, pivot_root, mount), `CAP_KILL` (signaling), `CAP_SYS_BOOT`
(reboot), `CAP_NET_ADMIN` (privileged-port socket activation). All others
dropped. Warns and continues with full caps if `capset` fails (sandbox
without `CAP_SETPCAP`).

Per-service `capabilities:` field is still deferred — the parser accepts
and skips it. Add when a real config asks.

#### 1.2 Resilient execve failures  ✅ shipped

Services with `start-condition:` that exit 127 (execve failure) re-arm
`cron_next = now + retry-after` (default 5s, configurable via
`retry-after: "Ns";`) up to `max_restarts` times, then `mark_failed`.
Services without `start-condition:` keep old behavior — immediate FAILED,
no respawn. Treats the failure as transient (e.g. filesystem not mounted
yet) only when the operator has declared a precondition.

#### 1.3 Journal integration  ✅ shipped

`openlog("hoshizora", LOG_PID|LOG_NDELAY, LOG_DAEMON)` once in `main()`.
Every `log_msg` now also calls `syslog(prio, ...)` alongside the existing
stderr + in-memory ring writes. No-op if `/dev/log` is missing — degraded
mode is acceptable. Pairs naturally with `hzlog` (the sibling binary) for
persistent collection. No custom journal format — `hzlog` already provides
persistence; reusing via `syslog(3)` is one call.

### Theme 2 — Usability & management

#### 2.1 Variable substitution  ✅ shipped

Top-level `$NAME = "value";` assignments (outside the `system` block) are
collected during parse and substituted into string-typed service fields
post-parse: `exec`, `log`, `listen`, `rootfs`, `bind`. Lexer change: `$`
is now a valid IDENT start char.

deferred: no IDENT substitution (only string fields), no conditionals
(`if/else`), no arithmetic, no includes. Add when a real config can't be
expressed without them.

#### 2.2 Startup ordering + timeouts  ✅ shipped

`timeout-start: "30s";` arms `start_deadline` at `start_service` time.
Checked in the 5s health tick. Disarmed on RUNNING, or on `READY=1` if
`expect-notify: true;` is set. Missed deadline → SIGTERM + `mark_failed`
+ `on-fail:` fires.

`start-after:` was dropped from the spec — `requires:` already implies
ordering, and `start-after:` without `requires:` is just a hint with no
enforcement semantics. Add when a real config needs decoupled
ordering-vs-dep.

#### 2.3 Service groups / targets  ✅ shipped

`target NAME { requires: [a, b, c]; }` defines a named collection.
`hzctl start <target>` walks the list and starts each member. Stored in
`g_sys.targets[]` alongside `services[]`.

deferred: `isolate` command (stop everything not in a target's dep
closure), target-on-target deps. Add when a real config has 10+ services
that need batch operations.

### Theme 3 — Modern infrastructure

#### 3.1 Socket activation  ✅ shipped

`listen: "host:port";` (TCP) or `listen: "/path.sock";` (Unix).
`setup_listen_sockets()` binds all at startup. Child dups fds to 3+ and
gets `HZ_LISTEN_FDS=N` env var. Lets services lazy-start on first
connection without losing the socket.

deferred: `LISTEN_FDS` env var per systemd convention (we use
`HZ_LISTEN_FDS` to avoid colliding with services expecting systemd's
exact semantics). Add when a real service needs systemd-style inheritance.

#### 3.2 sd-notify (readiness protocol)  ✅ shipped

Services with `expect-notify: true;` get `HZ_NOTIFY_SOCKET=/run/hoshizora/notify`
in their env. They send `<service-name> READY=1` or `STOPPING=1`
datagrams to that socket (override via `HZ_NOTIFY_PATH`). Hoshizora adds
the socket as the 6th poll fd, parses incoming datagrams, disarms
`start_deadline` on READY.

deferred: full `sd_notify` protocol (status text, errno reporting,
watchdog, mainpid). Just READY + STOPPING. Add fields when a service uses
them.

#### 3.3 Container integration  ✅ shipped

`namespace: private;` calls `unshare(CLONE_NEWNS|CLONE_NEWNET|NEWPID)` in
the child post-fork and brings up loopback in the new netns.
`rootfs: "/path";` bind-mounts and `pivot_root`s. `bind: "src:dst";`
creates mount entries before exec. All degrade to non-fatal warnings if
capabilities are missing.

deferred: image format, layer storage, OCI runtime compat. That's
containerd's job. Hoshizora just supervises the resulting process tree.

---

## v2.0 final numbers

| Metric | v1.x | v2.0 |
|---|---|---|
| LOC (init.c) | 1,872 | 2,382 |
| LOC (hzctl.c) | 64 | 64 |
| LOC (hzlog.c) | 97 | 97 |
| LOC (include/hoshizora.h) | 121 | 157 |
| Total project LOC | ~2,540 | ~2,700 |
| External deps | libc | libc (capset via raw syscall) |
| Build tools | gcc + make | gcc + make |
| Binary size (hoshizora) | ~950 KB | ~1.0 MB |
| RSS | <1 MiB | <1 MiB |
| Boot overhead | <5 ms | <5 ms |
| Self-checks | 5 (47 PASS) | 6 (55 PASS) |
| Config fields beyond v1.0 | +3 (every, retry-after, timeout-start, expect-notify, listen, namespace, rootfs, bind, target, $vars) | +10 |

---

## Explicitly NOT building

- **`boot0.asm`** — kernel already loads the ELF, sets up paging/GDT/stack.
- **`.hsb` bytecode + `bytegen` compiler** — text parse is <1ms once, not worth 2,000 LOC.
- **Custom libc subset** — glibc static is fine, 800 KiB on 4 GB RAM is 0.02%.
- **Slab allocator** — three mallocs at boot, never freed, gives same determinism.
- **`io_uring`** — `poll()` over 6 fds is sub-ms.
- **Starfield (full-fs fanotify + 64 MiB mmap log + seccomp TRACE)** — auditd's job.
- **Snapshot & Resume** — unimplementable as specified; CRIU's job.
- **GDT/IDT re-init** — kernel's job.
- **`memory-high`, `io-weight`** cgroup fields — add when a config uses them.
- **Cron-syntax `0 3 * * *`** — date math, defer until a real config asks.
- **Per-facility syslog split, log rotation in `hzlog`** — `awk` and `logrotate` already do this.
- **Custom journal format** — `hzlog` already provides persistence; reusing via `syslog(3)` is one call.
- **`isolate` target command** — stop everything not in a target's dep closure. Add when needed.
- **`start-after:`** — `requires:` already implies ordering. Add when decoupled hint is needed.
- **Per-service capabilities drop** — parser accepts and skips. Add when a config asks.
- **OCI image format / layer storage** — containerd's job. Hoshizora supervises processes.
- **systemd-style `LISTEN_FDS` env var** — we use `HZ_LISTEN_FDS` to avoid colliding with services expecting exact systemd semantics.

## Real-init concerns that are services, not PID 1 features

- getty / tty1 — service
- networking — service (`exec: "ip" with args ["link", "set", "lo", "up"]`)
- fstab mount — service (`exec: "mount" with args ["-a"]`)
- udev coldplug — service (`exec: "udevadm" with args ["trigger"]`)
- hostname — service (`exec: "hostname" with args ["myhost"]`)
- module load — service (`exec: "modprobe" with args ["ext4"]`)
- RTC sync — service (`exec: "hwclock" with args ["--hctosys"]`)
- swap on — service (`exec: "swapon" with args ["-a"]`)

Hoshizora is the supervisor. Everything else is a service.

---

## Final CLI surface (v2.0 — current)

```
hzctl list                       # all services + state
hzctl status [name]              # one or all
hzctl start <name|target>        # start a stopped service or all members of a target
hzctl stop <name>                # stop + block respawn
hzctl restart <name>             # stop + start
hzctl reload <name>              # SIGHUP the service (per-service reload)
hzctl reload                     # daemon-reload (re-read config)
hzctl daemon-reload              # explicit alias for above
hzctl enable <name>              # mark for autostart (default state)
hzctl disable <name>             # skip at boot / reload
hzctl show                       # list services + enabled state
hzctl logs [N]                   # last N log lines (default 50)
hzctl shutdown                   # power off (sync + reboot(RB_POWER_OFF))
hzctl poweroff                   # alias for shutdown
hzctl reboot                     # restart (sync + reboot(RB_AUTOBOOT))
hzctl help                       # this list
```

Future candidates (deferred until triggered):
```
hzctl isolate <target>           # start target, stop everything else
hzctl status <target>            # target's services as a group
```
