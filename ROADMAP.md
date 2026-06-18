# Hoshizora Roadmap

Ship what's actually needed. No new pillars, no new backends, no new file
formats, no new build tools, no new subsystems — until a real config asks
for them.

## Design principles

- **Modular** — features that can be compiled out or disabled at runtime.
- **Transparent** — readable code, predictable behavior. One process, one
  loop, seven fds.
- **Minimal by default** — the core ships only what a real config needs.
  Optional features are marked `deferred:` in source and added when a
  concrete use case arrives.

## What's shipped

See `README.md` for the full feature list (31 items across v1.x–v2.2).
`git log` for the history. This file is forward-looking only.

## Deferred — add when triggered

| Feature | Trigger | Est. LOC |
|---|---|---|
| `isolate <target>` command | 10+ services in a real config | ~20 |
| `start-after:` ordering hint | config needs decoupled hint-vs-dep | ~15 |
| `stop-when-unneeded:` | reverse-dep tracking needed | ~30 |
| Cron-syntax `0 3 * * *` | real config needs calendar scheduling | ~50 |
| Per-service capability drop | config asks for `capabilities:` field | ~40 |
| `memory-high`, `io-weight` cgroup fields | Already shipped v2.2 | — |
| `exec-probe`, `http-probe` | tcp-probe doesn't cover a use case | ~20 each |
| inotify fallback (when fanotify lacks CAP_SYS_ADMIN) | real non-root deployment | ~50 |
| OCI runtime compat / image format | deployment wants to drop containerd | ~200+ |
| system-level `LISTEN_FDS` env (systemd compat) | service expects exact systemd semantics | ~10 |
| NEWUSER namespace (uid/gid mapping) | config needs user-namespace isolation | ~30 |
| LVM / RAID / LUKS in initramfs-init | real boot needs encrypted/complex root | ~50+ |
| `hzctl --json` output | automation needs machine-readable output | ~30 |
| `hzctl top` live status | operator wants real-time view | ~80 |
| Man pages (`man hoshizora`, `man hzctl`, `man system.hs`) | real install on a real box | ~200 |
| Bootable ISO builder | distribution wants a downloadable image | ~50 |

## Explicitly NOT building

- **io_uring** — poll() over 7 fds is sub-ms.
- **eventfd replacing timerfd/signalfd** — eventfd can't do absolute-time fire or signal info.
- **Custom journal format** — hzlog + syslog(3) covers persistence.
- **Cluster mode** — different domain entirely.
- **Snapshot/resume** — CRIU's job.
- **bootloader / custom libc / slab allocator / .hsb bytecode** — all dropped. Kernel + musl + text config is enough.

## CLI surface (v2.3 — current)

SOV (Subject-Object-Verb) is the only order. `<name> <action>`.

```
hzctl <name> start | stop | restart | reload | status | enable | disable
hzctl <target> start
hzctl list | status [<name>] | show | reload | daemon-reload | logs [N]
hzctl shutdown [--force] | poweroff | reboot | help
```

`hzctl-systemctl` wrapper translates `systemctl start X` → `hzctl X start`.

Bash/zsh completions in `scripts/`.
