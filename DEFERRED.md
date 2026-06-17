# Deferred Features Ledger

Every deliberate `deferred:` shortcut in the codebase, in one place, so a
deferral can't quietly become permanent. Each marker in source code is
tagged with `deferred:` and names its ceiling plus the upgrade path.

**82 markers, 80 with no concrete trigger yet.** Add the feature when a
real config asks for it; until then, the deferral is intentional and the
shortcut stays.

v2.0 shipped nine features that were previously on this list (capability
dropping, resilient execve, journal integration, variable substitution,
startup timeouts, sd-notify, socket activation, container integration,
service targets). v2.1 shipped three more (plugin event socket, session
tracker, shutdown gate). Their `deferred:` markers in source have been
replaced with `v2.0:` / `v2.1:` markers. The remaining markers below are
still deferred.

See `ROADMAP.md` for forward-looking features that aren't yet
deferred-markers because they have agreed triggers and a shipping
priority.

## `hzctl.c` (1 marker)

- **`hzctl.c:2`** — one file, one job. Connects to a Unix socket, sends one line. `no-trigger`

## `include/hoshizora.h` (12 markers)

- **`include/hoshizora.h:3`** — minimum viable init. Parses system.hs, forks/execs services,. `no-trigger`
- **`include/hoshizora.h:28`** — start-condition supports 3 builtins, joined by `and` (max 2). `no-trigger`
- **`include/hoshizora.h:45`** — healthy: tcp-probe("host:port", Ns) only. One builtin. `no-trigger`
- **`include/hoshizora.h:52`** — on-fail: restart(name) | shutdown. Fires when service transitions. `no-trigger`
- **`include/hoshizora.h:78`** — presence of respawn: directive. `no-trigger`
- **`include/hoshizora.h:79`** — from backoff(max=N), default 5. `no-trigger`
- **`include/hoshizora.h:85`** — write memory.oom.group=1 — kill whole cgroup on OOM, not one proc. `no-trigger`
- **`include/hoshizora.h:86`** — file-exists / link-up / fs-mounted. `no-trigger`
- **`include/hoshizora.h:87`** — tcp-probe only. `no-trigger`
- **`include/hoshizora.h:88`** — per-service stdout/stderr redirect; empty = inherit. `no-trigger`
- **`include/hoshizora.h:89`** — action when service goes FAILED. `no-trigger`
- **`include/hoshizora.h:103`** — `recursive` keyword accepted in config for compatibility but. `no-trigger`

## `init.c` (56 markers)

- **`init.c:14`** — add when asked): io_uring (poll() over 4 fds suffices),.
- **`init.c:58`** — cgroup v2 magic — from <linux/magic.h>. Hard-coded to avoid the. `no-trigger`
- **`init.c:74`** — 8 KiB ring, byte-addressed with wrap. Walked backward for tail-N. `no-trigger`
- **`init.c:253`** — skip an unknown field's value inside a service/watch block. `no-trigger`
- **`init.c:270`** — parse size suffixes for memory-limit. Returns 0 on parse error. `no-trigger`
- **`init.c:287`** — generic over element size — used for both deps[HZ_MAX_NAME]. `no-trigger`
- **`init.c:383`** — extract max=N into max_restarts. base= ignored —. `no-trigger`
- **`init.c:408`** — lexer splits `256MiB` into NUMBER "256" + IDENT "MiB". `no-trigger`
- **`init.c:440`** — oom-kill: group; — writes memory.oom.group=1 in cgroup. `no-trigger`
- **`init.c:451`** — log: "path"; — redirect service stdout+stderr to this. `no-trigger`
- **`init.c:464`** — parse up to 2 builtins joined by `and`. Grammar:. `no-trigger`
- **`init.c:499`** — healthy: tcp-probe("host:port", Ns) only. `no-trigger`
- **`init.c:523`** — on-fail: restart(name) | shutdown;. `no-trigger`
- **`init.c:545`** — skip unknown field (capabilities, transactional,. `no-trigger`
- **`init.c:586`** — no. `no-trigger`
- **`init.c:659`** — optional `recursive` keyword — accepted for config. `no-trigger`
- **`init.c:682`** — all other idents (system, intents, starfield,. `no-trigger`
- **`init.c:719`** — 3 builtins, AND-only. tcp-probe via connect() with SO_SNDTIMEO. `no-trigger`
- **`init.c:725`** — operstate says "unknown" for lo on Linux (not "up"), so. `no-trigger`
- **`init.c:767`** — tcp-probe — connect() with SO_SNDTIMEO. Returns 0 on success. `no-trigger`
- **`init.c:787`** — ephemeral enable/disable state. A file at <ctl-dir>/enabled/<name>. `no-trigger`
- **`init.c:808`** — virtual intents — built-in dep names that resolve to runtime. `no-trigger`
- **`init.c:845`** — on-fail dispatcher. Called from mark_failed() whenever a service. `no-trigger`
- **`init.c:864`** — single FAILED-transition site → on-fail fires consistently. `no-trigger`
- **`init.c:876`** — ephemeral enable/disable marker. `disable X` writes the file;. `no-trigger`
- **`init.c:884`** — start-condition — if false, skip start (stay STOPPED). `no-trigger`
- **`init.c:896`** — not a real service — check virtual intents. `no-trigger`
- **`init.c:927`** — cgroup v2 — create per-service dir, write limits. No-op if. `no-trigger`
- **`init.c:936`** — parent blocked SIGCHLD/SIGTERM/SIGINT/SIGHUP for signalfd. `no-trigger`
- **`init.c:941`** — per-service log redirect. log_path empty = inherit. `no-trigger`
- **`init.c:990`** — linear wait. Add timed SIGKILL if services ignore SIGTERM. `no-trigger`
- **`init.c:1008`** — signals handled via signalfd in the main poll loop — see. `no-trigger`
- **`init.c:1017`** — mkdir per-service dir, write memory.max + cpu.weight, assign child. `no-trigger`
- **`init.c:1029`** — would log per-service-per-start; check at startup instead. `no-trigger`
- **`init.c:1056`** — oom-kill: group — write 1 to memory.oom.group so the kernel. `no-trigger`
- **`init.c:1105`** — exit 127 = execve failed (ENOENT/EACCES/etc). Respawning. `no-trigger`
- **`init.c:1116`** — state → STOPPED so start_service will accept the. `no-trigger`
- **`init.c:1121`** — linear backoff = restart_count seconds, capped at 30. ceiling: 30. `no-trigger`
- **`init.c:1140`** — parallel stop — SIGTERM all, wait once (5s), SIGKILL stragglers. `no-trigger`
- **`init.c:1185`** — shutdown_all = build reverse list of all services, call stop_parallel. `no-trigger`
- **`init.c:1200`** — signalfd integrates signals into the poll loop — no async-signal. `no-trigger`
- **`init.c:1219`** — env var override for tests; default /run/hoshizora/control. `no-trigger`
- **`init.c:1245`** — 0660 — root-only by default. Add a hoshizora group + chgrp. `no-trigger`
- **`init.c:1254`** — CLOCK_MONOTONIC — immune to NTP step adjustments and avoids. `no-trigger`
- **`init.c:1262`** — monotonic clock in seconds — used for respawn_at. `no-trigger`
- **`init.c:1272`** — separate timerfd fired every 5s for health probes. Reuses the. `no-trigger`
- **`init.c:1284`** — probe every RUNNING service with health.hostport set. `no-trigger`
- **`init.c:1314`** — needs CAP_SYS_ADMIN. If init isn't running as. `no-trigger`
- **`init.c:1355`** — walk fixed-size event metadata records. `no-trigger`
- **`init.c:1394`** — text protocol, one connection per command, single-line response. `no-trigger`
- **`init.c:1422`** — top-level commands that take no service-name arg in slot 1. `no-trigger`
- **`init.c:1579`** — snapshot running pids by name, re-read, walk new config to adopt. `no-trigger`
- **`init.c:1595`** — spec changes that affect the running process — restart on diff. `no-trigger`
- **`init.c:1613`** — snapshot pre-reload services (full structs) — used for both. `no-trigger`
- **`init.c:1757`** — one-time warning if cgroup v2 isn't available but config asks. `no-trigger`
- **`init.c:1811`** — PID 1 returning from main = kernel panic ("Attempted to kill. `no-trigger`

## `Makefile` (2 markers)

- **`Makefile:2`** — one binary, no bootloader, no host compiler, no .hsb step. `no-trigger`
- **`Makefile:19`** — ONE testsuite entry point. Runs each self-check in sequence,. `no-trigger`

## `README.md` (1 marker)

- **`README.md:60`** — ` markers in `init.c` call these out. Add when actually needed:.

## `ROADMAP.md` (1 marker)

- **`ROADMAP.md:250`** — )". `no-trigger`

## `tests/core.hs` (1 marker)

- **`tests/core.hs:19`** — exec-failure test — bad path, child exits 127. Should go to. `no-trigger`

## `tests/core.sh` (1 marker)

- **`tests/core.sh:4`** — one shell script, no test framework. `no-trigger`

## `tests/features.hs` (2 markers)

- **`tests/features.hs:30`** — on-fail: restart(limited_service) — if dep_service goes. `no-trigger`
- **`tests/features.hs:36`** — network_ready is a VIRTUAL intent — no service block defines. `no-trigger`

## `tests/onfail.sh` (1 marker)

- **`tests/onfail.sh:3`** — one config, one assertion path — service crashes past max_restarts,. `no-trigger`

## `tests/testsuite.sh` (1 marker)

- **`tests/testsuite.sh:3`** — one shell script, no test framework. Runs each self-check in. `no-trigger`
