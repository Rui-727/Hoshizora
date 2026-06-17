/*
 * HOSHIZORA — init system (PID 1) for Linux/x86_64.
 *
 * Real PID 1: parses system.hs, forks/execs services in dependency order,
 * restarts crashed ones with linear backoff, exposes a control socket for
 * runtime operations (list/start/stop/restart/reload/shutdown), reloads
 * config via SIGHUP or `reload` command (diffs + applies).
 *
 * Implemented: cgroups v2 (memory.max + cpu.weight), fanotify file watches,
 * start-condition (file-exists / link-up / fs-mounted, AND-only), health
 * probes (tcp-probe), parallel shutdown + reload, in-memory log ring,
 * enable/disable ephemeral state, poweroff/reboot via reboot(2).
 *
 * Skipped (deferred: add when asked): io_uring (poll() over 4 fds suffices),
 * transactional snapshot, capabilities, non-root operator access.
 *
 * Build: make
 * Run:   ./hoshizora [system.hs]
 * Ctl:   hzctl list  (or: hzctl <name> <action> — Gentoo-style)
 */

#include "hoshizora.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/statfs.h>
#include <sys/fanotify.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <sys/prctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

/* v2.0: capability dropping via raw capset(2) syscall. Inline the structs
 * instead of including <linux/capability.h> — musl doesn't ship kernel
 * headers, and we only need the ABI structs + version constant. */
#define HZ_LINUX_CAPABILITY_VERSION_3 0x20080522
struct hz_user_cap_header_struct { unsigned int version; int pid; };
struct hz_user_cap_data_struct { unsigned int effective, permitted, inheritable; };

static hz_system_t g_sys;
static int g_sigfd = -1;
static int g_ctlfd = -1;
static int g_reloadfd = -1;   /* timerfd armed for next pending respawn */
static int g_healthfd = -1;   /* timerfd armed every 5s for health probes */
static int g_fanfd = -1;      /* fanotify fd for file watches; -1 if unused */
static int g_notifyfd = -1;   /* v2.0: sd_notify socket; -1 if unused */
static int g_eventfd = -1;    /* v2.1: plugin event broadcast socket; -1 if unused */
static volatile sig_atomic_t g_shutdown;
static int g_reboot_target;  /* 0 = poweroff, 1 = reboot. Set by `reboot` cmd. */
static const char *g_cfg_path;
static char g_ctl_path[256] = "/run/hoshizora/control";
static char g_notify_path[256] = "/run/hoshizora/notify";
static char g_event_path[256] = "/run/hoshizora/events";

/* v2.1: plugin event subscribers. Fixed-size array of accepted client fds
 * that stay open and receive broadcast event lines. Each subscriber has a
 * per-client ring buffer (HZ_SUB_BUF bytes) — event_broadcast appends to
 * each subscriber's buffer, then flushes via non-blocking send(). If the
 * buffer fills, the event is dropped (with a one-shot warning per
 * subscriber) so a slow plugin can't lag the main poll loop. */
#define HZ_MAX_SUBS 8
#define HZ_SUB_BUF  4096   /* per-subscriber ring buffer. 4 KiB absorbs ~68
                            * events of avg 60 bytes — enough headroom for a
                            * briefly-stalled plugin without unbounded growth.
                            * A truly stuck plugin overflows in seconds and
                            * gets drop-warnings; that's the right behavior. */
struct hz_sub {
    int  fd;
    char buf[HZ_SUB_BUF];
    int  head;       /* next byte to send */
    int  tail;       /* next byte to write */
    int  dropped;    /* 1 = warning already logged for current drop burst */
};
static struct hz_sub g_subs[HZ_MAX_SUBS];
static int g_n_subs;

/* v2.0: variable substitution. Top-level $NAME = "value" pairs, substituted
 * deferred: only in TOK_STRING (not IDENT) — covers paths in exec/log/listen.
 * Add IDENT substitution if a real config needs it. */
static char g_var_keys[HZ_MAX_VARS][HZ_MAX_NAME];
static char g_var_vals[HZ_MAX_VARS][HZ_MAX_STR];
static int  g_n_vars;

/* deferred: cgroup v2 magic — from <linux/magic.h>. Hard-coded to avoid the
 * extra include. */
#define HZ_CGROUP2_SUPER_MAGIC 0x63677270
#define HZ_CGROUP_BASE "/sys/fs/cgroup/hoshizora"

/* forward decls — cgroup helpers are defined below, used by start/stop */
static int  cgroup_v2_available(void);
static void cgroup_setup_for(const hz_service_t *s);
static void cgroup_assign_pid(const hz_service_t *s, pid_t pid);
static void cgroup_teardown_for(const hz_service_t *s);
static hz_service_t *find_service(const char *name);
static hz_target_t  *find_target(const char *name);
static int start_service(hz_service_t *s);
static int  stop_service(hz_service_t *s);
static void stop_parallel(hz_service_t **list, int n, const char *why);  /* deferred: stop_service delegates here */
static void subst_vars(char *buf, size_t bufsz);  /* v2.0: $NAME → value */
static int  setup_listen_sockets(void);           /* v2.0: bind socket-activation fds */
static int  setup_notify_socket(void);            /* v2.0: bind /run/hoshizora/notify */
static void handle_notify_event(void);            /* v2.0: parse READY=1, disarm start_deadline */
/* v2.0 helpers used before definition */
static int  parse_duration(const char *s);
static void mkdir_parents_of(const char *path);
static int  parse_hostport(const char *str, char *host_out, int host_sz, int *port_out);
/* v2.1: socket helpers used before definition */
static int  setup_unix_socket(const char *env_name, const char *default_path,
                              int sock_type, int do_listen,
                              char *path_out, size_t path_sz, const char *label);

/* v2.1: plugin event socket */
static int  setup_event_socket(void);
static void event_broadcast(const char *fmt, ...);
static void handle_event_subscriber(void);
static void event_flush_all(void);
static void sub_remove(int idx);
static time_t mono_now(void);  /* v2.2: needed by handle_notify_event + start_service */

/* ---------------------------------------------------------------------------
 * LOGGING — writes to stderr AND an in-memory ring buffer for `hzctl logs`.
 * deferred: 8 KiB ring, byte-addressed with wrap. Walked backward for tail-N.
 * ------------------------------------------------------------------------- */
static void ctl_send(int fd, const char *s);  /* forward decl — defined in CONTROL SOCKET section */
#define HZ_LOG_RING_SIZE 8192
static char g_log_ring[HZ_LOG_RING_SIZE];
static int  g_log_ring_pos;   /* next write offset (wraps) */
static int  g_log_ring_used;  /* bytes currently in ring, capped at size */

static void log_ring_append(const char *buf, int n) {
    if (n >= HZ_LOG_RING_SIZE) {
        memcpy(g_log_ring, buf + (n - HZ_LOG_RING_SIZE), HZ_LOG_RING_SIZE);
        g_log_ring_pos  = 0;
        g_log_ring_used = HZ_LOG_RING_SIZE;
        return;
    }
    int first = HZ_LOG_RING_SIZE - g_log_ring_pos;
    if (n <= first) {
        memcpy(g_log_ring + g_log_ring_pos, buf, n);
    } else {
        memcpy(g_log_ring + g_log_ring_pos, buf, first);
        memcpy(g_log_ring, buf + first, n - first);
    }
    g_log_ring_pos = (g_log_ring_pos + n) % HZ_LOG_RING_SIZE;
    if (g_log_ring_used < HZ_LOG_RING_SIZE) g_log_ring_used += n;
}

static void log_msg(const char *level, const char *fmt, ...) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[hoshizora %s] ", level);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    n = strlen(buf);
    if (n < (int)sizeof(buf) - 1) { buf[n++] = '\n'; buf[n] = 0; }
    (void)write(2, buf, n);
    log_ring_append(buf, n);
    /* v2.0: forward to syslog (hzlog collects /dev/log). No-op if /dev/log
     * is missing. Pass the formatted message directly via vsyslog so the
     * format string + args are processed by syslog, not sliced from buf
     * with a magic offset (was buf+14 — brittle if prefix format changes). */
    int prio = LOG_INFO;
    if (level[0] == 'W') prio = LOG_WARNING;
    else if (level[0] == 'E') prio = LOG_ERR;
    va_start(ap, fmt);
    vsyslog(prio, fmt, ap);
    va_end(ap);
}
#define LOGI(...) log_msg("I", __VA_ARGS__)
#define LOGW(...) log_msg("W", __VA_ARGS__)
#define LOGE(...) log_msg("E", __VA_ARGS__)

/* ---------------------------------------------------------------------------
 * v2.0: CAPABILITY DROPPING — raw capset(2) syscall, no libcap dependency.
 * Drops all capabilities except what PID 1 actually needs:
 *   CAP_SYS_ADMIN  — cgroup mkdir/write, pivot_root, mount
 *   CAP_KILL       — signaling services
 *   CAP_SYS_BOOT   — reboot(2)
 *   CAP_NET_ADMIN  — bind to ports < 1024 (socket activation)
 *   CAP_SYS_PTRACE — not needed, drop
 * deferred: per-service capabilities: field — add when a config asks. */
static int drop_capabilities(void) {
    struct hz_user_cap_header_struct hdr = { .version = HZ_LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    struct hz_user_cap_data_struct data[2] = {0};
    /* bit 21 = CAP_SYS_ADMIN, 5 = CAP_KILL, 22 = CAP_SYS_BOOT, 12 = CAP_NET_ADMIN */
    unsigned int keep = (1U << 21) | (1U << 5) | (1U << 22) | (1U << 12);
    data[0].effective = data[0].permitted = data[0].inheritable = keep;
    /* bits 32+ → data[1]; none of our kept caps are there */
    if (syscall(SYS_capset, &hdr, data) < 0) {
        LOGW("capset: %s — running with full caps", strerror(errno));
        return -1;
    }
    LOGI("dropped capabilities (kept: sys_admin, kill, sys_boot, net_admin)");
    return 0;
}

/* ---------------------------------------------------------------------------
 * v2.0: VARIABLE SUBSTITUTION — replace $NAME with value in a string buffer.
 * Top-level $NAME = "value" pairs collected during parse. Only applied to
 * TOK_STRING values (paths, exec, log, listen). deferred: no IDENT subst. */
static void subst_vars(char *buf, size_t bufsz) {
    if (g_n_vars == 0) return;
    char out[HZ_MAX_STR * 2];
    size_t oi = 0;
    size_t i = 0;
    size_t len = strlen(buf);
    while (i < len && oi < sizeof(out) - 1) {
        if (buf[i] == '$' && i + 1 < len) {
            /* extract IDENT after $ */
            size_t j = i + 1;
            size_t k = 0;
            char name[HZ_MAX_NAME];
            while (j < len && k < sizeof(name) - 1 &&
                   ((buf[j] >= 'a' && buf[j] <= 'z') ||
                    (buf[j] >= 'A' && buf[j] <= 'Z') ||
                    (buf[j] >= '0' && buf[j] <= '9') || buf[j] == '_')) {
                name[k++] = buf[j++];
            }
            name[k] = 0;
            if (k == 0) { out[oi++] = buf[i++]; continue; }
            /* look up */
            const char *val = NULL;
            for (int v = 0; v < g_n_vars; v++) {
                if (strcmp(g_var_keys[v], name) == 0) { val = g_var_vals[v]; break; }
            }
            if (val) {
                size_t vl = strlen(val);
                if (oi + vl >= sizeof(out) - 1) break;
                memcpy(out + oi, val, vl); oi += vl;
                i = j;
                continue;
            }
            /* unknown var — leave as-is */
            out[oi++] = buf[i++];
        } else {
            out[oi++] = buf[i++];
        }
    }
    out[oi] = 0;
    strncpy(buf, out, bufsz - 1);
    buf[bufsz - 1] = 0;
}

/* ---------------------------------------------------------------------------
 * v2.0: SOCKET ACTIVATION — bind listening sockets at startup, pass via fd 3+
 * to started services. deferred: LISTEN_FDS env var per systemd convention
 * (we use a custom env HZ_LISTEN_FDS to avoid colliding with real systemd
 * services that expect specific fd-passing semantics). */
static int setup_listen_sockets(void) {
    int bound = 0;
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *s = &g_sys.services[i];
        for (int j = 0; j < HZ_MAX_LISTENS && s->listens[j][0]; j++) {
            int fd = -1;
            const char *addr = s->listens[j];
            if (addr[0] == '/') {
                /* unix socket */
                unlink(addr);
                fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (fd < 0) { LOGW("%s: listen socket: %s", s->name, strerror(errno)); continue; }
                struct sockaddr_un un = {0};
                un.sun_family = AF_UNIX;
                strncpy(un.sun_path, addr, sizeof(un.sun_path) - 1);
                if (bind(fd, (struct sockaddr*)&un, sizeof(un)) < 0 || listen(fd, 8) < 0) {
                    LOGW("%s: bind %s: %s", s->name, addr, strerror(errno));
                    close(fd); continue;
                }
            } else {
                /* tcp — parse "host:port" */
                char host[64]; int port;
                if (parse_hostport(addr, host, sizeof(host), &port) != 0) {
                    LOGW("%s: bad listen: %s", s->name, addr); continue;
                }
                fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (fd < 0) continue;
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                struct sockaddr_in sa = {0};
                sa.sin_family = AF_INET;
                sa.sin_port = htons((uint16_t)port);
                if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
                    LOGW("%s: bad listen host: %s", s->name, host); close(fd); continue;
                }
                if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
                    LOGW("%s: bind %s: %s", s->name, addr, strerror(errno));
                    close(fd); continue;
                }
            }
            if (s->n_listen_fds < HZ_MAX_LISTENS) {
                s->listen_fds[s->n_listen_fds++] = fd;
                bound++;
                LOGI("%s: listening on %s (fd=%d)", s->name, addr, fd);
            } else {
                close(fd);
            }
        }
    }
    return bound;
}

/* v2.1: helper — bind a Unix socket at `path` (env-override via `env_name`).
 * Used by notify + event sockets (both 0666, any UID can connect — they're
 * public input paths from untrusted services/plugins). Control socket stays
 * inline because it uses 0660 (root-only). sock_type = SOCK_STREAM or
 * SOCK_DGRAM. do_listen = 1 calls listen(). Returns bound fd or -1. */
static int setup_unix_socket(const char *env_name, const char *default_path,
                             int sock_type, int do_listen,
                             char *path_out, size_t path_sz,
                             const char *label) {
    const char *env = env_name ? getenv(env_name) : NULL;
    if (env && *env) {
        strncpy(path_out, env, path_sz - 1);
        path_out[path_sz - 1] = 0;
    } else {
        strncpy(path_out, default_path, path_sz - 1);
        path_out[path_sz - 1] = 0;
    }
    mkdir_parents_of(path_out);
    unlink(path_out);
    int fd = socket(AF_UNIX, sock_type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { LOGW("%s socket: %s", label, strerror(errno)); return -1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(path_out) >= sizeof(addr.sun_path)) {
        LOGW("%s path too long", label); close(fd); return -1;
    }
    strcpy(addr.sun_path, path_out);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0
        || (do_listen && listen(fd, 8) < 0)) {
        LOGW("%s bind: %s", label, strerror(errno));
        close(fd); return -1;
    }
    chmod(path_out, 0666);
    LOGI("%s socket: %s", label, path_out);
    return fd;
}

/* ---------------------------------------------------------------------------
 * v2.0: sd-notify — one global Unix datagram socket at /run/hoshizora/notify.
 * Services send "<service-name> READY=1" or "<service-name> STOPPING=1".
 * deferred: full sd_notify protocol (status text, errno, watchdog). */
static int setup_notify_socket(void) {
    /* v2.0: sd-notify socket, datagram (services send READY=1 etc.). */
    g_notifyfd = setup_unix_socket("HZ_NOTIFY_PATH", "/run/hoshizora/notify",
                                   SOCK_DGRAM, 0,
                                   g_notify_path, sizeof(g_notify_path), "notify");
    return g_notifyfd < 0 ? -1 : 0;
}

static void handle_notify_event(void) {
    char buf[512];
    ssize_t n = recv(g_notifyfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = 0;
    /* parse "<name> READY=1" — name is up to first space, then key=val */
    char *sp = strchr(buf, ' ');
    if (!sp) return;
    *sp = 0;
    char *rest = sp + 1;
    hz_service_t *s = find_service(buf);
    if (!s) return;
    if (strstr(rest, "READY=1")) {
        if (!s->notify_ready) {
            s->notify_ready = 1;
            s->start_deadline = 0;  /* disarm timeout */
            LOGI("%s: ready (sd_notify)", s->name);
            event_broadcast("READY service=%s\n", s->name);
        }
    } else if (strstr(rest, "STOPPING=1")) {
        LOGI("%s: stopping (sd_notify)", s->name);
        event_broadcast("STOPPING service=%s\n", s->name);
    } else if (strstr(rest, "WATCHDOG=1")) {
        /* v2.2: watchdog — service is alive, reset the timer. */
        s->watchdog_last = mono_now();
    } else if (strstr(rest, "ERRNO=") || strstr(rest, "BUSERROR=")) {
        /* v2.2: notify-failure — service reports why it's about to fail.
         * Log the full message; mark_failed fires when the process exits. */
        LOGW("%s: sd_notify failure: %s", s->name, rest);
        event_broadcast("NOTIFY_FAILURE service=%s msg=%s\n", s->name, rest);
    }
}

/* ---------------------------------------------------------------------------
 * v2.1: PLUGIN EVENT SOCKET — broadcast state changes to subscriber processes.
 * Plugins connect to /run/hoshizora/events and stay open; hoshizora writes
 * event lines like "START service=nginx pid=1234" to all subscribers.
 * deferred: no per-client buffering, no flow control, no event filtering.
 * Slow subscriber blocks broadcasts to all. Add per-client queues if needed. */
static int setup_event_socket(void) {
    g_eventfd = setup_unix_socket("HZ_EVENT_PATH", "/run/hoshizora/events",
                                  SOCK_STREAM, 1,
                                  g_event_path, sizeof(g_event_path), "event");
    return g_eventfd < 0 ? -1 : 0;
}

static void handle_event_subscriber(void) {
    int cfd = accept4(g_eventfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) return;
    if (g_n_subs < HZ_MAX_SUBS) {
        struct hz_sub *s = &g_subs[g_n_subs];
        memset(s, 0, sizeof(*s));
        s->fd = cfd;
        g_n_subs++;
        /* HELLO is queued via the same buffer path so a slow subscriber
         * doesn't block accept() either. */
        event_broadcast("HELLO hoshizora subscribers=%d\n", g_n_subs);
        LOGI("event subscriber accepted (total=%d)", g_n_subs);
    } else {
        dprintf(cfd, "ERROR too many subscribers\n");
        close(cfd);
    }
}

/* v2.1: remove a subscriber by index (swap-remove). Used by event_broadcast
 * and event_flush_all when a subscriber's socket is gone. */
static void sub_remove(int idx) {
    LOGI("event subscriber fd=%d gone", g_subs[idx].fd);
    close(g_subs[idx].fd);
    g_subs[idx] = g_subs[g_n_subs - 1];
    g_n_subs--;
}

/* v2.1: flush a subscriber's ring buffer via non-blocking send(). Returns
 * 0 on success (or would-block), -1 if the subscriber is gone (broken pipe). */
static int sub_flush(struct hz_sub *s) {
    while (s->head != s->tail) {
        int len = (s->tail > s->head) ? (s->tail - s->head)
                                       : (HZ_SUB_BUF - s->head);
        ssize_t w = send(s->fd, s->buf + s->head, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w > 0) {
            s->head = (s->head + w) % HZ_SUB_BUF;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        /* EPIPE / ECONNRESET / other → subscriber gone */
        return -1;
    }
    /* buffer empty — reset head/tail to 0 to maximize contiguous space */
    s->head = s->tail = 0;
    return 0;
}

/* v2.1: append data to a subscriber's ring buffer. If the buffer is full,
 * drop the event (mark s->dropped so we warn once per burst). Returns 0
 * on success, -1 if the subscriber should be removed (gone). */
static int sub_append(struct hz_sub *s, const char *data, int n) {
    /* try to flush first — maybe there's room after a send */
    if (sub_flush(s) < 0) return -1;
    int used = (s->tail - s->head + HZ_SUB_BUF) % HZ_SUB_BUF;
    int free = HZ_SUB_BUF - used - 1;  /* -1 to distinguish full from empty */
    if (n > free) {
        if (!s->dropped) {
            LOGW("event subscriber fd=%d buffer full — dropping events", s->fd);
            s->dropped = 1;
        }
        return 0;  /* don't kill the subscriber, just drop */
    }
    /* append, handling wrap */
    int first = HZ_SUB_BUF - s->tail;
    if (n <= first) {
        memcpy(s->buf + s->tail, data, n);
    } else {
        memcpy(s->buf + s->tail, data, first);
        memcpy(s->buf, data + first, n - first);
    }
    s->tail = (s->tail + n) % HZ_SUB_BUF;
    s->dropped = 0;  /* successful append resets the drop-warning state */
    /* try to send immediately — if it would block, the data stays buffered */
    if (sub_flush(s) < 0) return -1;
    return 0;
}

static void event_broadcast(const char *fmt, ...) {
    if (g_n_subs == 0) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    /* walk subscribers; remove any that are gone (broken pipe / closed) */
    int i = 0;
    while (i < g_n_subs) {
        if (sub_append(&g_subs[i], buf, n) < 0) {
            sub_remove(i);
            continue;  /* don't increment i — new sub at same slot */
        }
        i++;
    }
}

/* v2.1: called once per main-loop iteration. Attempts to flush any pending
 * buffered events to subscribers whose kernel-side socket buffer may have
 * drained. Cheap: 8 non-blocking send() calls, mostly EAGAIN immediately. */
static void event_flush_all(void) {
    int i = 0;
    while (i < g_n_subs) {
        if (sub_flush(&g_subs[i]) < 0) {
            sub_remove(i);
            continue;
        }
        i++;
    }
}

/* ---------------------------------------------------------------------------
 * v2.0: CONTAINER INTEGRATION — unshare + pivot_root + bind mounts.
 * Called in child post-fork, pre-exec.  deferred: no image format, no OCI. */
static int setup_container_child(const hz_service_t *s) {
    if (!s->container.new_ns && !s->container.rootfs[0]) return 0;
    if (s->container.new_ns) {
        /* v2.2: full namespace isolation — added NEWIPC|NEWUTS. deferred: NEWUSER
         * needs uid/gid mapping setup, skip until a real config asks. */
        if (unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS) < 0) {
            LOGW("%s: unshare: %s — running without namespace isolation", s->name, strerror(errno));
            /* keep going — degraded mode, not fatal */
        }
        /* deferred: bring up loopback in the new netns. Requires struct ifreq
         * from <net/if.h> — including it just for one ioctl is heavier than
         * the call is worth. Add when a real config uses namespace: private
         * AND needs local networking inside the namespace. */
    }
    if (s->container.rootfs[0]) {
        /* mount rootfs as private so pivot_root doesn't propagate */
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
        /* bind-mount rootfs onto itself so we can pivot */
        if (mount(s->container.rootfs, s->container.rootfs, NULL, MS_BIND, NULL) < 0) {
            LOGW("%s: bind rootfs %s: %s", s->name, s->container.rootfs, strerror(errno));
            return -1;
        }
        char putold[HZ_MAX_PATH + 8];
        snprintf(putold, sizeof(putold), "%s/put_old", s->container.rootfs);
        mkdir(putold, 0755);
        if (syscall(SYS_pivot_root, s->container.rootfs, putold) < 0) {
            LOGW("%s: pivot_root: %s — running in parent ns", s->name, strerror(errno));
            return 0;
        }
        chdir("/");
        umount2("/put_old", MNT_DETACH);
        rmdir("/put_old");
    }
    /* bind mounts — applied after pivot if rootfs is set */
    for (int i = 0; i < s->container.n_binds; i++) {
        char src[HZ_MAX_PATH], dst[HZ_MAX_PATH];
        if (sscanf(s->container.binds[i], "%255[^:]:%255s", src, dst) != 2) continue;
        if (mount(src, dst, NULL, MS_BIND, NULL) < 0) {
            LOGW("%s: bind %s→%s: %s", s->name, src, dst, strerror(errno));
        }
    }
    return 0;
}

/* dump last n_lines log lines to control client. Walks the ring backward
 * counting newlines, then forward-writes the resulting slice. */
static void logs_dump(int cfd, int n_lines) {
    if (g_log_ring_used == 0) { ctl_send(cfd, "(no logs)"); return; }
    if (n_lines <= 0) n_lines = 50;
    /* walk backward from g_log_ring_pos-1, count newlines, stop at n_lines */
    int count = 0;
    int pos = (g_log_ring_pos - 1 + HZ_LOG_RING_SIZE) % HZ_LOG_RING_SIZE;
    int remaining = g_log_ring_used;
    int last_nl = -1;
    while (remaining > 0) {
        if (g_log_ring[pos] == '\n') {
            count++;
            last_nl = pos;
            if (count >= n_lines) break;
        }
        pos = (pos - 1 + HZ_LOG_RING_SIZE) % HZ_LOG_RING_SIZE;
        remaining--;
    }
    int dump_start, dump_len;
    if (count >= n_lines && last_nl >= 0) {
        dump_start = (last_nl + 1) % HZ_LOG_RING_SIZE;
    } else {
        /* fewer than n_lines in ring — dump everything */
        dump_start = (g_log_ring_pos - g_log_ring_used + HZ_LOG_RING_SIZE) % HZ_LOG_RING_SIZE;
    }
    dump_len = (g_log_ring_pos - dump_start + HZ_LOG_RING_SIZE) % HZ_LOG_RING_SIZE;
    if (dump_len == 0) { ctl_send(cfd, "(no logs)"); return; }
    /* write in up to two parts (wrap) */
    if (dump_start + dump_len <= HZ_LOG_RING_SIZE) {
        (void)write(cfd, g_log_ring + dump_start, dump_len);
    } else {
        int first = HZ_LOG_RING_SIZE - dump_start;
        (void)write(cfd, g_log_ring + dump_start, first);
        (void)write(cfd, g_log_ring, dump_len - first);
    }
    /* ensure trailing newline */
    int last = (g_log_ring_pos - 1 + HZ_LOG_RING_SIZE) % HZ_LOG_RING_SIZE;
    if (g_log_ring[last] != '\n') (void)write(cfd, "\n", 1);
}

/* ---------------------------------------------------------------------------
 * CONFIG PARSER — token-stream walker for the system.hs subset.
 * Recognizes: service NAME { exec, requires, respawn, environment }.
 * Ignores everything else (intents, watch, starfield, memory-limit, etc.)
 * with a deferred comment marking them as YAGNI.
 * ------------------------------------------------------------------------- */

typedef enum {
    TOK_EOF, TOK_IDENT, TOK_STRING, TOK_NUMBER, TOK_PUNCT
} tok_kind_t;

typedef struct {
    tok_kind_t kind;
    char       text[HZ_MAX_STR];
} tok_t;

typedef struct {
    const char *p;
    const char *end;
} lexer_t;

static void lex_init(lexer_t *L, const char *src, size_t len) {
    L->p = src; L->end = src + len;
}

static void skip_ws(lexer_t *L) {
    while (L->p < L->end) {
        char c = *L->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { L->p++; continue; }
        if (c == '#') { while (L->p < L->end && *L->p != '\n') L->p++; continue; }
        break;
    }
}

static int next_token(lexer_t *L, tok_t *t) {
    skip_ws(L);
    if (L->p >= L->end) { t->kind = TOK_EOF; t->text[0] = 0; return 0; }
    char c = *L->p;

    /* string */
    if (c == '"') {
        L->p++;
        size_t i = 0;
        while (L->p < L->end && *L->p != '"' && i < sizeof(t->text) - 1) {
            if (*L->p == '\\' && L->p + 1 < L->end) {
                L->p++;
                char e = *L->p++;
                t->text[i++] = (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
            } else {
                t->text[i++] = *L->p++;
            }
        }
        if (L->p < L->end && *L->p == '"') L->p++;
        t->text[i] = 0;
        t->kind = TOK_STRING;
        return 1;
    }

    /* number */
    if ((c >= '0' && c <= '9') || c == '-') {
        size_t i = 0;
        if (c == '-') t->text[i++] = *L->p++;
        while (L->p < L->end && *L->p >= '0' && *L->p <= '9' && i < sizeof(t->text) - 1)
            t->text[i++] = *L->p++;
        t->text[i] = 0;
        t->kind = TOK_NUMBER;
        return 1;
    }

    /* identifier — v2.0: `$` is a valid start char for top-level variable
     * names ($NAME = "value"). deferred: no other special chars. */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$') {
        size_t i = 0;
        /* consume the start char (c) */
        t->text[i++] = *L->p++;
        while (L->p < L->end &&
               ((*L->p >= 'a' && *L->p <= 'z') || (*L->p >= 'A' && *L->p <= 'Z') ||
                (*L->p >= '0' && *L->p <= '9') || *L->p == '_' || *L->p == '-') &&
               i < sizeof(t->text) - 1)
            t->text[i++] = *L->p++;
        t->text[i] = 0;
        t->kind = TOK_IDENT;
        return 1;
    }

    /* punctuation: single char */
    t->text[0] = c; t->text[1] = 0;
    t->kind = TOK_PUNCT;
    L->p++;
    return 1;
}

static int peek_ident_is(lexer_t *L, const char *kw) {
    lexer_t save = *L;
    tok_t t;
    next_token(&save, &t);
    return t.kind == TOK_IDENT && strcmp(t.text, kw) == 0;
}

/* deferred: skip an unknown field's value inside a service/watch block.
 * Stops at: top-level ';' (statement terminator), OR unmatched '}'/']'/')'
 * at depth 0 (block close — rewind one char so caller's loop sees it).
 * Tracks {}/[]/() nesting so a `[ ... ]` inside the value doesn't end early. */
static void skip_unknown_field(lexer_t *L) {
    tok_t t;
    int depth = 0;
    for (;;) {
        if (!next_token(L, &t)) break;
        if (t.kind == TOK_PUNCT && (t.text[0]=='{'||t.text[0]=='['||t.text[0]=='(')) depth++;
        else if (t.kind == TOK_PUNCT && (t.text[0]=='}'||t.text[0]==']'||t.text[0]==')')) {
            if (depth == 0) { L->p--; break; }
            depth--;
        } else if (t.kind == TOK_PUNCT && t.text[0]==';' && depth == 0) break;
    }
}

/* v2.0: skip a balanced { ... } block. Caller has NOT consumed the opening
 * `{`. Reads tokens until the matching `}` (depth tracking handles nested
 * blocks), then returns. If an unmatched `}` appears at depth 0, rewinds
 * so the caller's loop sees it. */
static void skip_brace_block(lexer_t *L) {
    tok_t t;
    int depth = 0;
    for (;;) {
        if (!next_token(L, &t)) break;
        if (t.kind == TOK_PUNCT && t.text[0] == '{') depth++;
        else if (t.kind == TOK_PUNCT && t.text[0] == '}') {
            if (depth == 0) { L->p--; break; }
            if (--depth == 0) break;
        }
    }
}

/* v2.0: parse a duration string like "30s", "5m", "2h". Returns seconds
 * or -1 on parse error. Used by every:, timeout-start:, retry-after:.
 * deferred: no `d` (days), no fractional values, no ms. */
static int parse_duration(const char *s) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return -1;
    unsigned long mul;
    if      (*end == 's' || *end == 0) mul = 1;
    else if (*end == 'm')              mul = 60;
    else if (*end == 'h')              mul = 3600;
    else return -1;
    return (int)(v * mul);
}

/* v2.0: mkdir -p for the parent of `path`. Walks each path component and
 * creates it. Used by control socket + notify socket + disable cmd. */
static void mkdir_parents_of(const char *path) {
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    if (!dir[0]) return;
    char *p = dir;
    while (*p) {
        if (*p == '/' && p > dir) {
            *p = 0; mkdir(dir, 0755); *p = '/';
        }
        p++;
    }
    mkdir(dir, 0755);
}

/* v2.0: parse "host:port" into separate buffers. Returns 0 on success,
 * -1 on error. host buffer must be at least 64 bytes. */
static int parse_hostport(const char *str, char *host_out, int host_sz, int *port_out) {
    if (sscanf(str, "%63[^:]:%d", host_out, port_out) != 2) return -1;
    if (*port_out < 1 || *port_out > 65535) return -1;
    (void)host_sz;  /* sscanf width already enforces */
    return 0;
}

/* deferred: parse size suffixes for memory-limit. Returns 0 on parse error.
 * Accepts: 256MiB, 1GiB, 512KiB, 1024 (bare = bytes). Decimal suffixes (KB/MB)
 * dropped — README documents binary only, cgroup v2 memory.max wants bytes. */
static unsigned long long parse_size(const char *s) {
    char *end;
    unsigned long long n = strtoull(s, &end, 10);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end == 0) return n;
    if (strcmp(end, "KiB") == 0) return n * 1024ULL;
    if (strcmp(end, "MiB") == 0) return n * 1024ULL * 1024;
    if (strcmp(end, "GiB") == 0) return n * 1024ULL * 1024 * 1024;
    if (strcmp(end, "TiB") == 0) return n * 1024ULL * 1024 * 1024 * 1024;
    return 0;
}

/* parse a string list: [ "a", "b" ] or [ ident, ident ] */
/* deferred: generic over element size — used for both deps[HZ_MAX_NAME]
 * and args[HZ_MAX_STR]. One function, two callers, no template needed. */
static int parse_string_list(lexer_t *L, void *out, size_t elem_size, int max, int *n) {
    char *base = (char*)out;
    tok_t t;
    if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '[') return -1;
    *n = 0;
    for (;;) {
        if (!next_token(L, &t)) return -1;
        if (t.kind == TOK_PUNCT && t.text[0] == ']') return 0;
        if (t.kind != TOK_STRING && t.kind != TOK_IDENT) return -1;
        if (strlen(t.text) >= elem_size) {
            LOGE("string list element too long (max %zu): %s", elem_size - 1, t.text);
            return -1;
        }
        if (*n < max) {
            strncpy(base + (*n) * elem_size, t.text, elem_size - 1);
            base[(*n) * elem_size + elem_size - 1] = 0;
            (*n)++;
        }
        if (!next_token(L, &t)) return -1;
        if (t.kind == TOK_PUNCT && t.text[0] == ']') return 0;
        if (t.kind != TOK_PUNCT || t.text[0] != ',') return -1;
    }
}

/* parse environment block: { "K": "V", "K2": "V2" } */
static int parse_env_block(lexer_t *L, hz_service_t *s) {
    tok_t t;
    if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '{') return -1;
    for (;;) {
        if (!next_token(L, &t)) return -1;
        if (t.kind == TOK_PUNCT && t.text[0] == '}') return 0;
        if (t.kind != TOK_STRING) return -1;
        if (s->n_env >= HZ_MAX_ENV) return -1;
        if (strlen(t.text) >= HZ_MAX_NAME) {
            LOGE("env key too long (max %d): %s", HZ_MAX_NAME - 1, t.text);
            return -1;
        }
        strncpy(s->env_keys[s->n_env], t.text, HZ_MAX_NAME - 1);
        s->env_keys[s->n_env][HZ_MAX_NAME - 1] = 0;
        if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
        if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
        strncpy(s->env_vals[s->n_env], t.text, HZ_MAX_STR - 1);
        s->env_vals[s->n_env][HZ_MAX_STR - 1] = 0;
        s->n_env++;
        if (!next_token(L, &t)) return -1;
        if (t.kind == TOK_PUNCT && (t.text[0] == ',' || t.text[0] == ';')) {
            if (t.text[0] == ';') {
                if (!next_token(L, &t)) return -1;
                if (t.kind == TOK_PUNCT && t.text[0] == '}') return 0;
            }
            continue;
        }
        if (t.kind == TOK_PUNCT && t.text[0] == '}') return 0;
        return -1;
    }
}

/* parse a single service block starting after the opening brace */
static int parse_service_block(lexer_t *L, hz_service_t *s) {
    tok_t t;
    for (;;) {
        if (!next_token(L, &t)) return -1;
        if (t.kind == TOK_PUNCT && t.text[0] == '}') return 0;
        if (t.kind == TOK_PUNCT && t.text[0] == ';') continue;  /* statement terminator */
        if (t.kind != TOK_IDENT) return -1;

        if (strcmp(t.text, "exec") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            strncpy(s->exec, t.text, HZ_MAX_PATH - 1);
            s->exec[HZ_MAX_PATH - 1] = 0;
            /* optional: with args [ ... ] */
            if (peek_ident_is(L, "with")) {
                next_token(L, &t); /* consume "with" */
                if (!next_token(L, &t) || t.kind != TOK_IDENT || strcmp(t.text, "args") != 0) return -1;
                char args[HZ_MAX_ARGS][HZ_MAX_STR];
                int n = 0;
                if (parse_string_list(L, args, HZ_MAX_STR, HZ_MAX_ARGS, &n) != 0) return -1;
                for (int i = 0; i < n && s->argc < HZ_MAX_ARGS; i++) {
                    strncpy(s->argv[s->argc], args[i], HZ_MAX_STR - 1);
                    s->argv[s->argc][HZ_MAX_STR - 1] = 0;
                    s->argc++;
                }
            }
            /* trailing ; (if any) handled by the for loop's statement-terminator skip */
            (void)t;
        } else if (strcmp(t.text, "requires") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (parse_string_list(L, s->deps, HZ_MAX_NAME, HZ_MAX_DEPS, &s->n_deps) != 0) return -1;
        } else if (strcmp(t.text, "respawn") == 0) {
            s->respawn = 1;
            s->max_restarts = 5;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            /* accept: backoff(max = N, base = Xs); or just ;
             * deferred: extract max=N into max_restarts. base= ignored —
             * respawn uses linear restart_count-second delay, capped at 30. */
            int depth = 0;
            int seen_max = 0;
            for (;;) {
                if (!next_token(L, &t)) break;
                if (t.kind == TOK_PUNCT && t.text[0] == '(') { depth++; continue; }
                if (t.kind == TOK_PUNCT && t.text[0] == ')') { depth--; continue; }
                if (t.kind == TOK_PUNCT && t.text[0] == ';' && depth == 0) break;
                /* capture N after `max = ` */
                if (t.kind == TOK_IDENT && strcmp(t.text, "max") == 0) seen_max = 1;
                else if (seen_max && t.kind == TOK_NUMBER) {
                    int v = atoi(t.text);
                    if (v > 0) s->max_restarts = v;
                    seen_max = 0;
                }
            }
        } else if (strcmp(t.text, "environment") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (parse_env_block(L, s) != 0) return -1;
            /* trailing ; (if any) handled by the for loop */
        } else if (strcmp(t.text, "memory-limit") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t)) return -1;
            /* accept STRING ("256MiB") or IDENT/NUMBER (256MiB unquoted).
             * deferred: lexer splits `256MiB` into NUMBER "256" + IDENT "MiB"
             * — re-combine so parse_size sees the suffix. */
            char val[HZ_MAX_STR] = {0};
            if (t.kind == TOK_STRING) {
                strncpy(val, t.text, sizeof(val) - 1);
            } else if (t.kind == TOK_NUMBER || t.kind == TOK_IDENT) {
                strncpy(val, t.text, sizeof(val) - 1);
                /* peek for following IDENT (the suffix) */
                lexer_t save = *L;
                tok_t t2;
                if (next_token(&save, &t2) && t2.kind == TOK_IDENT) {
                    size_t vl = strlen(val), sl = strlen(t2.text);
                    if (vl + sl < sizeof(val)) {
                        strcat(val, t2.text);
                        *L = save;  /* consume the suffix token */
                    }
                }
            } else return -1;
            s->memory_limit = parse_size(val);
            if (s->memory_limit == 0) {
                LOGE("%s: bad memory-limit: %s", s->name, val);
                return -1;
            }
        } else if (strcmp(t.text, "cpu-weight") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_NUMBER) return -1;
            s->cpu_weight = atoi(t.text);
            if (s->cpu_weight < 1 || s->cpu_weight > 10000) {
                LOGE("%s: cpu-weight %s out of range (1-10000)", s->name, t.text);
                return -1;
            }
        } else if (strcmp(t.text, "oom-kill") == 0) {
            /* deferred: oom-kill: group; — writes memory.oom.group=1 in cgroup.
             * No value variant (oom-kill: process) — group is the only sane
             * choice for a supervised service. Add when a config needs the
             * per-process behavior. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT || strcmp(t.text, "group") != 0) {
                LOGE("%s: oom-kill: only `group` supported", s->name);
                return -1;
            }
            s->oom_kill_group = 1;
        } else if (strcmp(t.text, "log") == 0) {
            /* deferred: log: "path"; — redirect service stdout+stderr to this
             * file. Default (no log: directive) is /var/log/hoshizora/<name>.log
             * if that dir exists, else inherit hoshizora's stderr. Empty string
             * disables redirection. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            if (strlen(t.text) >= HZ_MAX_PATH) {
                LOGE("%s: log path too long: %s", s->name, t.text);
                return -1;
            }
            strncpy(s->log_path, t.text, HZ_MAX_PATH - 1);
            s->log_path[HZ_MAX_PATH - 1] = 0;
        } else if (strcmp(t.text, "start-condition") == 0) {
            /* deferred: parse up to 2 builtins joined by `and`. Grammar:
             *   start-condition: BUILTIN ( "arg" ) [ and BUILTIN ( "arg" ) ] ;
             * BUILTIN ∈ { file-exists, link-up, fs-mounted }. Anything else
             * (arithmetic, if/then/else, OR) → parse error. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            hz_sc_kind_t *pkind = &s->start_cond.kind1;
            char (*parg)[HZ_MAX_PATH] = &s->start_cond.arg1;
            int slot = 0;
            for (;;) {
                if (!next_token(L, &t) || t.kind != TOK_IDENT) return -1;
                if      (strcmp(t.text, "file-exists") == 0) *pkind = HZ_SC_FILE_EXISTS;
                else if (strcmp(t.text, "link-up") == 0)     *pkind = HZ_SC_LINK_UP;
                else if (strcmp(t.text, "fs-mounted") == 0)  *pkind = HZ_SC_FS_MOUNTED;
                else { LOGE("%s: unknown start-condition builtin: %s", s->name, t.text); return -1; }
                if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '(') return -1;
                if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
                if (strlen(t.text) >= HZ_MAX_PATH) {
                    LOGE("%s: start-condition arg too long: %s", s->name, t.text);
                    return -1;
                }
                strncpy(*parg, t.text, HZ_MAX_PATH - 1);
                (*parg)[HZ_MAX_PATH - 1] = 0;
                if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ')') return -1;
                /* optional `and <builtin>` — only one level deep */
                if (peek_ident_is(L, "and")) {
                    next_token(L, &t); /* consume `and` */
                    if (++slot > 1) { LOGE("%s: start-condition supports max 2 builtins", s->name); return -1; }
                    pkind = &s->start_cond.kind2;
                    parg  = &s->start_cond.arg2;
                    continue;
                }
                if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
                break;
            }
        } else if (strcmp(t.text, "healthy") == 0) {
            /* deferred: healthy: tcp-probe("host:port", Ns) only. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT || strcmp(t.text, "tcp-probe") != 0) {
                LOGE("%s: healthy: only tcp-probe(...) supported", s->name);
                return -1;
            }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '(') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            if (strlen(t.text) >= HZ_MAX_STR) {
                LOGE("%s: healthy host:port too long: %s", s->name, t.text);
                return -1;
            }
            strncpy(s->health.hostport, t.text, HZ_MAX_STR - 1);
            s->health.hostport[HZ_MAX_STR - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ',') return -1;
            if (!next_token(L, &t) || t.kind != TOK_NUMBER) return -1;
            /* v2.3: accept "5s" form — lexer splits into NUMBER "5" + IDENT "s".
             * Peek and consume the trailing IDENT if it's a single-char unit. */
            lexer_t save = *L; tok_t t2;
            if (next_token(&save, &t2) && t2.kind == TOK_IDENT && strlen(t2.text) <= 2) {
                *L = save;  /* consume the unit token */
            }
            s->health.timeout_s = atoi(t.text);
            if (s->health.timeout_s < 1 || s->health.timeout_s > 60) {
                LOGE("%s: healthy timeout %s out of range (1-60s)", s->name, t.text);
                return -1;
            }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ')') return -1;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "on-fail") == 0) {
            /* deferred: on-fail: restart(name) | shutdown;
             * Two builtins, no args beyond the target name. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT) return -1;
            if (strcmp(t.text, "shutdown") == 0) {
                s->on_fail.kind = HZ_ON_FAIL_SHUTDOWN;
            } else if (strcmp(t.text, "restart") == 0) {
                if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '(') return -1;
                if (!next_token(L, &t) || t.kind != TOK_IDENT) return -1;
                if (strlen(t.text) >= HZ_MAX_NAME) {
                    LOGE("%s: on-fail target too long: %s", s->name, t.text);
                    return -1;
                }
                strcpy(s->on_fail.target, t.text);
                s->on_fail.kind = HZ_ON_FAIL_RESTART;
                if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ')') return -1;
            } else {
                LOGE("%s: on-fail: only restart(...) or shutdown", s->name);
                return -1;
            }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "every") == 0) {
            /* deferred: every: "Ns"; — interval in seconds. Service becomes a cron
             * job: one-shot fork+exec, re-arms on clean exit. Cron-syntax
             * `0 3 * * *` deferred — needs date math, add when a real config
             * asks for it. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            s->cron_interval = parse_duration(t.text);
            if (s->cron_interval < 1) { LOGE("%s: bad every: %s", s->name, t.text); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "timeout-start") == 0) {
            /* v2.0: timeout-start: Ns — if not RUNNING within N seconds, FAILED. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            s->timeout_start = parse_duration(t.text);
            if (s->timeout_start < 1) { LOGE("%s: bad timeout-start: %s", s->name, t.text); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "retry-after") == 0) {
            /* v2.0: retry-after: Ns — services with start-condition that fail
             * to execve (exit 127) re-arm cron_next = now + retry_after. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            s->retry_after = parse_duration(t.text);
            if (s->retry_after < 1) { LOGE("%s: bad retry-after: %s", s->name, t.text); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "expect-notify") == 0) {
            /* v2.0: expect-notify: true; — service speaks sd_notify. Pairs
             * with timeout-start; timer disarms on READY=1. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT || strcmp(t.text, "true") != 0) {
                LOGE("%s: expect-notify: only `true` supported", s->name); return -1;
            }
            s->expect_notify = 1;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "listen") == 0) {
            /* v2.0: listen: "addr"; — socket activation. Hoshizora binds,
             * child inherits fd via HZ_LISTEN_FDS env var. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            int idx = 0;
            while (idx < HZ_MAX_LISTENS && s->listens[idx][0]) idx++;
            if (idx >= HZ_MAX_LISTENS) { LOGE("%s: too many listen: entries", s->name); return -1; }
            strncpy(s->listens[idx], t.text, HZ_MAX_STR - 1);
            s->listens[idx][HZ_MAX_STR - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "namespace") == 0) {
            /* v2.0: namespace: private; — unshare mount+net+pid namespaces. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT || strcmp(t.text, "private") != 0) {
                LOGE("%s: namespace: only `private` supported", s->name); return -1;
            }
            s->container.new_ns = 1;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "rootfs") == 0) {
            /* v2.0: rootfs: "/path"; — pivot_root into this dir before exec. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            strncpy(s->container.rootfs, t.text, HZ_MAX_PATH - 1);
            s->container.rootfs[HZ_MAX_PATH - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "bind") == 0) {
            /* v2.0: bind: "src:dst"; — bind-mount src onto dst in child ns. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            if (s->container.n_binds < HZ_MAX_BINDS) {
                strncpy(s->container.binds[s->container.n_binds], t.text, HZ_MAX_PATH - 1);
                s->container.binds[s->container.n_binds][HZ_MAX_PATH - 1] = 0;
                s->container.n_binds++;
            } else { LOGE("%s: too many bind: entries", s->name); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "rootfs-readonly") == 0) {
            /* v2.2: rootfs-readonly: true; — remount rootfs MS_RDONLY after pivot. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT || strcmp(t.text, "true") != 0) {
                LOGE("%s: rootfs-readonly: only `true` supported", s->name); return -1;
            }
            s->container.readonly = 1;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "io-weight") == 0) {
            /* v2.2: io-weight: N (1-10000); — cgroup v2 io.weight */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_NUMBER) return -1;
            s->io_weight = atoi(t.text);
            if (s->io_weight < 1 || s->io_weight > 10000) {
                LOGE("%s: io-weight %s out of range (1-10000)", s->name, t.text); return -1;
            }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "memory-high") == 0) {
            /* v2.2: memory-high: 512MiB; — cgroup v2 memory.high (soft limit) */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t)) return -1;
            char val[HZ_MAX_STR] = {0};
            if (t.kind == TOK_STRING) strncpy(val, t.text, sizeof(val) - 1);
            else if (t.kind == TOK_NUMBER || t.kind == TOK_IDENT) {
                strncpy(val, t.text, sizeof(val) - 1);
                lexer_t save = *L; tok_t t2;
                if (next_token(&save, &t2) && t2.kind == TOK_IDENT) {
                    size_t vl = strlen(val), sl = strlen(t2.text);
                    if (vl + sl < sizeof(val)) { strcat(val, t2.text); *L = save; }
                }
            } else return -1;
            s->memory_high = parse_size(val);
            if (s->memory_high == 0) { LOGE("%s: bad memory-high: %s", s->name, val); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "cpu-max") == 0) {
            /* v2.2: cpu-max: "50%"; or "quota period" — cgroup v2 cpu.max.
             * "50%" → quota = period * 50 / 100 (period default 100000us). */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            if (strchr(t.text, '%')) {
                int pct = atoi(t.text);
                int period = 100000;
                s->cpu_max_quota = (int)((long long)period * pct / 100);
                s->cpu_max_period = period;
            } else if (sscanf(t.text, "%d %d", &s->cpu_max_quota, &s->cpu_max_period) >= 1) {
                if (s->cpu_max_period == 0) s->cpu_max_period = 100000;
            } else { LOGE("%s: bad cpu-max: %s", s->name, t.text); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "no-new-privs") == 0) {
            /* v2.2: no-new-privs: true; — prctl(PR_SET_NO_NEW_PRIVS) in child. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT || strcmp(t.text, "true") != 0) {
                LOGE("%s: no-new-privs: only `true` supported", s->name); return -1;
            }
            s->no_new_privs = 1;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "run-as") == 0) {
            /* v2.2: run-as: "uid:gid"; — setuid/setgid before exec. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            unsigned int u, g;
            if (sscanf(t.text, "%u:%u", &u, &g) != 2) {
                LOGE("%s: bad run-as: %s (expected 'uid:gid')", s->name, t.text); return -1;
            }
            s->run_as_uid = u; s->run_as_gid = g;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "pre-start") == 0) {
            /* v2.2: pre-start: "cmd"; — shell command run before fork. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            strncpy(s->pre_start, t.text, HZ_MAX_PATH - 1);
            s->pre_start[HZ_MAX_PATH - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "post-stop") == 0) {
            /* v2.2: post-stop: "cmd"; — shell command run after stop reaps. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            strncpy(s->post_stop, t.text, HZ_MAX_PATH - 1);
            s->post_stop[HZ_MAX_PATH - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else if (strcmp(t.text, "watchdog-timeout") == 0) {
            /* v2.2: watchdog-timeout: Ns; — service must send WATCHDOG=1
             * within this interval or be marked FAILED. */
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_STRING) return -1;
            s->watchdog_timeout = parse_duration(t.text);
            if (s->watchdog_timeout < 1) { LOGE("%s: bad watchdog-timeout: %s", s->name, t.text); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
        } else {
            /* deferred: skip unknown field (capabilities, transactional,
             * snapshot). Add when a feature is actually requested. */
            skip_unknown_field(L);
        }
    }
}

/* parse a watch block: on-change: reload(name) | restart(name) */
static int parse_watch_block(lexer_t *L, hz_watch_t *w) {
    tok_t t;
    for (;;) {
        if (!next_token(L, &t)) return -1;
        if (t.kind == TOK_PUNCT && t.text[0] == '}') return 0;
        if (t.kind == TOK_PUNCT && t.text[0] == ';') continue;
        if (t.kind != TOK_IDENT) return -1;
        if (strcmp(t.text, "on-change") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT) return -1;
            if (strcmp(t.text, "reload") == 0) w->action = HZ_W_RELOAD;
            else if (strcmp(t.text, "restart") == 0) w->action = HZ_W_RESTART;
            else { LOGE("watch %s: unknown action %s", w->path, t.text); return -1; }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '(') return -1;
            if (!next_token(L, &t) || t.kind != TOK_IDENT) return -1;
            if (strlen(t.text) >= HZ_MAX_NAME) {
                LOGE("watch service name too long: %s", t.text);
                return -1;
            }
            strncpy(w->service, t.text, HZ_MAX_NAME - 1);
            w->service[HZ_MAX_NAME - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ')') return -1;
        } else {
            LOGW("watch %s: unknown field %s — skipping", w->path, t.text);
            skip_unknown_field(L);
        }
    }
}

/* parse top-level: walk tokens, only act on `service IDENT {` and
 * `watch STRING [recursive] {`, ignore everything else. Non-service/watch
 * constructs (system "name" {...}, intents {...}, starfield {...}) are
 * skipped naturally — their braces and strings get filtered as non-IDENT
 * tokens, their closing } is consumed by the punctuation case. deferred: no
 * depth tracking needed. */
static int parse_config(lexer_t *L) {
    tok_t t;
    while (next_token(L, &t)) {
        if (t.kind == TOK_EOF) return 0;
        if (t.kind != TOK_IDENT) continue;  /* skip strings, punctuation, numbers */

        /* v2.0: top-level variable assignment: $NAME = "value";
         * deferred: only string values, no expansion in idents. */
        if (t.text[0] == '$') {
            char key[HZ_MAX_NAME];
            strncpy(key, t.text + 1, sizeof(key) - 1);
            key[sizeof(key) - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '=') {
                LOGE("variable %s: expected '='", t.text); return -1;
            }
            if (!next_token(L, &t) || t.kind != TOK_STRING) {
                LOGE("variable %s: expected string value", key); return -1;
            }
            if (g_n_vars < HZ_MAX_VARS) {
                strncpy(g_var_keys[g_n_vars], key, HZ_MAX_NAME - 1);
                g_var_keys[g_n_vars][HZ_MAX_NAME - 1] = 0;
                strncpy(g_var_vals[g_n_vars], t.text, HZ_MAX_STR - 1);
                g_var_vals[g_n_vars][HZ_MAX_STR - 1] = 0;
                g_n_vars++;
            }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ';') return -1;
            continue;
        }
        if (strcmp(t.text, "service") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_IDENT) {
                LOGE("service: expected name");
                return -1;
            }
            if (g_sys.n_services >= HZ_MAX_SERVICES) {
                LOGW("too many services (max %d), ignoring %s", HZ_MAX_SERVICES, t.text);
                skip_brace_block(L);
                continue;
            }
            hz_service_t *s = &g_sys.services[g_sys.n_services];
            memset(s, 0, sizeof(*s));
            if (strlen(t.text) >= HZ_MAX_NAME) {
                LOGE("service name too long (max %d): %s", HZ_MAX_NAME - 1, t.text);
                return -1;
            }
            strncpy(s->name, t.text, HZ_MAX_NAME - 1);
            s->name[HZ_MAX_NAME - 1] = 0;
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '{') {
                LOGE("service %s: expected '{'", s->name);
                return -1;
            }
            if (parse_service_block(L, s) != 0) {
                LOGE("service %s: parse error", s->name);
                return -1;
            }
            if (s->exec[0] == 0) {
                LOGW("service %s: no exec: — skipping", s->name);
                continue;
            }
            g_sys.n_services++;
            LOGI("loaded service %s (exec=%s, %d deps, respawn=%d max=%d, mem=%lluB, cpu=%d%s%s%s%s%s)",
                 s->name, s->exec, s->n_deps, s->respawn, s->max_restarts,
                 (unsigned long long)s->memory_limit, s->cpu_weight,
                 s->oom_kill_group ? ", oom-group" : "",
                 s->start_cond.kind1 != HZ_SC_NONE ? ", start-cond" : "",
                 s->health.hostport[0] ? ", healthy" : "",
                 s->log_path[0] ? ", log" : "",
                 s->on_fail.kind != HZ_ON_FAIL_NONE ? ", on-fail" : "");
        } else if (strcmp(t.text, "watch") == 0) {
            if (!next_token(L, &t) || t.kind != TOK_STRING) {
                LOGE("watch: expected path string");
                return -1;
            }
            if (g_sys.n_watches >= HZ_MAX_WATCHES) {
                LOGW("too many watches (max %d), ignoring %s", HZ_MAX_WATCHES, t.text);
                skip_brace_block(L);
                continue;
            }
            hz_watch_t *w = &g_sys.watches[g_sys.n_watches];
            memset(w, 0, sizeof(*w));
            if (strlen(t.text) >= HZ_MAX_PATH) {
                LOGE("watch path too long (max %d): %s", HZ_MAX_PATH - 1, t.text);
                return -1;
            }
            strncpy(w->path, t.text, HZ_MAX_PATH - 1);
            w->path[HZ_MAX_PATH - 1] = 0;
            /* v2.2: optional `recursive` keyword — now honored via
             * FAN_MARK_FILESYSTEM (kernel ≥5.16), falls back to top-dir on
             * older kernels. */
            if (peek_ident_is(L, "recursive")) {
                next_token(L, &t);  /* consume `recursive` */
                w->recursive = 1;
            }
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '{') {
                LOGE("watch %s: expected '{'", w->path);
                return -1;
            }
            if (parse_watch_block(L, w) != 0) {
                LOGE("watch %s: parse error", w->path);
                return -1;
            }
            if (w->service[0] == 0) {
                LOGW("watch %s: no on-change action — ignoring", w->path);
                continue;
            }
            g_sys.n_watches++;
            LOGI("loaded watch %s -> %s %s", w->path,
                 w->action == HZ_W_RELOAD ? "reload" : "restart",
                 w->service);
            continue;
        }
        if (strcmp(t.text, "target") == 0) {
            /* v2.0: target NAME { requires: [a, b, c]; } — named service group.
             * hzctl start <target> walks the list. deferred: no isolate, no
             * target-on-target deps. */
            if (!next_token(L, &t) || t.kind != TOK_IDENT) {
                LOGE("target: expected name"); return -1;
            }
            if (g_sys.n_targets >= HZ_MAX_TARGETS) {
                LOGW("too many targets (max %d), ignoring %s", HZ_MAX_TARGETS, t.text);
                skip_brace_block(L);
                continue;
            }
            hz_target_t *tg = &g_sys.targets[g_sys.n_targets];
            memset(tg, 0, sizeof(*tg));
            strncpy(tg->name, t.text, HZ_MAX_NAME - 1);
            if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != '{') {
                LOGE("target %s: expected '{'", tg->name); return -1;
            }
            /* parse target body: only `requires:` field */
            for (;;) {
                if (!next_token(L, &t)) return -1;
                if (t.kind == TOK_PUNCT && t.text[0] == '}') break;
                if (t.kind == TOK_PUNCT && t.text[0] == ';') continue;
                if (t.kind != TOK_IDENT) return -1;
                if (strcmp(t.text, "requires") == 0) {
                    if (!next_token(L, &t) || t.kind != TOK_PUNCT || t.text[0] != ':') return -1;
                    if (parse_string_list(L, tg->services, HZ_MAX_NAME, HZ_MAX_DEPS, &tg->n_services) != 0) return -1;
                } else {
                    skip_unknown_field(L);
                }
            }
            g_sys.n_targets++;
            LOGI("loaded target %s (%d services)", tg->name, tg->n_services);
            continue;
        }
        /* deferred: all other idents (system, intents, starfield,
         * on-change, reload, restart, recursive, enabled, log-size,
         * exclude-paths, every, max, base, backoff, capabilities,
         * transactional, snapshot, start-condition,
         * healthy) — either handled inside parse_service_block or
         * parse_watch_block (when inside a block) or intentionally ignored
         * (when top-level or inside intents/starfield). Add handling when a
         * feature is actually requested. */
    }
    return 0;
}

static int load_config(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { LOGE("open %s: %s", path, strerror(errno)); return -1; }
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) { LOGE("read %s: %s", path, strerror(errno)); return -1; }
    buf[n] = 0;
    lexer_t L;
    lex_init(&L, buf, (size_t)n);
    int rc = parse_config(&L);
    if (rc != 0) return rc;
    /* v2.0: apply variable substitution to string-typed fields after parse.
     * deferred: no IDENT subst (only paths/strings). */
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *s = &g_sys.services[i];
        subst_vars(s->exec, sizeof(s->exec));
        subst_vars(s->log_path, sizeof(s->log_path));
        for (int j = 0; j < HZ_MAX_LISTENS && s->listens[j][0]; j++)
            subst_vars(s->listens[j], sizeof(s->listens[j]));
        subst_vars(s->container.rootfs, sizeof(s->container.rootfs));
        for (int j = 0; j < s->container.n_binds; j++)
            subst_vars(s->container.binds[j], sizeof(s->container.binds[j]));
        /* v2.2: subst lifecycle hooks too */
        subst_vars(s->pre_start, sizeof(s->pre_start));
        subst_vars(s->post_stop, sizeof(s->post_stop));
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * SERVICE LIFECYCLE
 * ------------------------------------------------------------------------- */

static hz_service_t *find_service(const char *name) {
    for (int i = 0; i < g_sys.n_services; i++)
        if (strcmp(g_sys.services[i].name, name) == 0) return &g_sys.services[i];
    return NULL;
}

/* v2.0: find a target by name. */
static hz_target_t *find_target(const char *name) {
    for (int i = 0; i < g_sys.n_targets; i++)
        if (strcmp(g_sys.targets[i].name, name) == 0) return &g_sys.targets[i];
    return NULL;
}

/* ---------------------------------------------------------------------------
 * START-CONDITION + HEALTH PROBES
 * deferred: 3 builtins, AND-only. tcp-probe via connect() with SO_SNDTIMEO.
 * ------------------------------------------------------------------------- */
static int sc_check_one(hz_sc_kind_t kind, const char *arg) {
    switch (kind) {
    case HZ_SC_FILE_EXISTS: return access(arg, F_OK) == 0;
    case HZ_SC_LINK_UP: {
        /* deferred: operstate says "unknown" for lo on Linux (not "up"), so
         * check the flags file for IFF_UP (0x1) instead — works for all ifaces. */
        char p[HZ_MAX_PATH + 64];
        snprintf(p, sizeof(p), "/sys/class/net/%s/flags", arg);
        int fd = open(p, O_RDONLY);
        if (fd < 0) return 0;
        char buf[32] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return 0;
        /* flags file is "0x<padded-hex>\n" — parse as hex, check IFF_UP bit */
        unsigned int flags = 0;
        sscanf(buf, "%x", &flags);
        return (flags & 0x1) != 0;
    }
    case HZ_SC_FS_MOUNTED: {
        /* walk /proc/mounts, look for arg as a mount point */
        FILE *f = fopen("/proc/mounts", "r");
        if (!f) return 0;
        char line[512];
        int found = 0;
        while (fgets(line, sizeof(line), f)) {
            char dev[256], mp[256];
            if (sscanf(line, "%255s %255s", dev, mp) >= 2) {
                if (strcmp(mp, arg) == 0) { found = 1; break; }
            }
        }
        fclose(f);
        return found;
    }
    case HZ_SC_NONE: return 1;
    }
    return 1;
}

static int evaluate_sc(const hz_sc_t *sc) {
    if (sc->kind1 == HZ_SC_NONE) return 1;
    if (!sc_check_one(sc->kind1, sc->arg1)) return 0;
    if (sc->kind2 != HZ_SC_NONE && !sc_check_one(sc->kind2, sc->arg2)) return 0;
    return 1;
}

/* deferred: tcp-probe — connect() with SO_SNDTIMEO. Returns 0 on success
 * (connection established), -1 on failure/timeout. Doesn't send/receive
 * anything — just checks the listener is alive. */
static int tcp_probe(const char *hostport, int timeout_s) {
    char host[128]; int port;
    if (parse_hostport(hostport, host, sizeof(host), &port) != 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return -1; }
    int r = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    close(fd);
    return r == 0 ? 0 : -1;
}

/* deferred: ephemeral enable/disable state. A file at <ctl-dir>/enabled/<name>
 * means "disabled". File absent = enabled. Tied to the control socket's parent
 * dir so it works in test mode (HZ_CTL_PATH override) too. Survives reload,
 * not reboot. For persistent enable/disable, edit the config. */
static void enabled_path_for(const char *name, char *out, size_t out_sz) {
    /* copy g_ctl_path, strip trailing /control, append /enabled/<name> */
    char dir[256];
    strncpy(dir, g_ctl_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else        dir[0] = 0;
    snprintf(out, out_sz, "%s/enabled/%s", dir, name);
}

static int service_enabled(const hz_service_t *s) {
    char p[HZ_MAX_PATH];
    enabled_path_for(s->name, p, sizeof(p));
    return access(p, F_OK) != 0;  /* file absent = enabled */
}

/* deferred: virtual intents — built-in dep names that resolve to runtime
 * conditions, not services. Currently just `network_ready`: true iff any
 * non-lo interface has IFF_UP set. system.hs uses `requires: [network_ready]`
 * without defining a network_ready service — this makes that work without
 * forcing the operator to write a dummy service.
 *
 * Returns 1 if `name` is a known virtual intent AND its condition is true.
 * Returns 0 if `name` is a known virtual intent AND its condition is false.
 * Returns -1 if `name` is not a virtual intent (caller should look it up as
 * a real service). */
static int virtual_dep_running(const char *name) {
    if (strcmp(name, "network_ready") != 0 && strcmp(name, "network-ready") != 0)
        return -1;
    /* walk /sys/class/net/ — any non-lo iface with IFF_UP (0x1) = ready */
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;
    int ready = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (strcmp(e->d_name, "lo") == 0) continue;
        char p[HZ_MAX_PATH + 64];
        snprintf(p, sizeof(p), "/sys/class/net/%s/flags", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        char buf[32] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        unsigned int flags = 0;
        sscanf(buf, "%x", &flags);
        if (flags & 0x1) { ready = 1; break; }
    }
    closedir(d);
    return ready;
}

/* deferred: on-fail dispatcher. Called from mark_failed() whenever a service
 * transitions to FAILED. RESTART stops + starts target; SHUTDOWN sets the
 * g_shutdown flag (main loop will pick it up next iteration). No cycle guard
 * — operator's config, their bug to see in logs. */
static time_t mono_now(void);               /* forward: defined below */
static void on_fail_trigger(hz_service_t *s) {
    if (s->on_fail.kind == HZ_ON_FAIL_NONE) return;
    if (s->on_fail.kind == HZ_ON_FAIL_SHUTDOWN) {
        LOGI("%s: on-fail: shutdown", s->name);
        g_shutdown = 1;
        return;
    }
    hz_service_t *t = find_service(s->on_fail.target);
    if (!t) { LOGW("%s: on-fail restart: no such service %s", s->name, s->on_fail.target); return; }
    LOGI("%s: on-fail: restart %s", s->name, t->name);
    if (t->state == HZ_S_RUNNING && t->pid > 0) stop_service(t);
    start_service(t);
}

/* deferred: single FAILED-transition site → on-fail fires consistently.
 * Replaces scattered `s->state = HZ_S_FAILED;` assignments. */
static void mark_failed(hz_service_t *s) {
    s->state = HZ_S_FAILED;
    event_broadcast("FAILED service=%s restarts=%d\n", s->name, s->restart_count);
    on_fail_trigger(s);
}

static int start_service(hz_service_t *s) {
    if (s->state == HZ_S_RUNNING) return 0;
    s->manual_stop = 0;
    s->respawn_at = 0;

    /* deferred: ephemeral enable/disable marker. `disable X` writes the file;
     * we skip start until `enable X` removes it. Operator can still `stop X`
     * while disabled — start is the only thing blocked. */
    if (!service_enabled(s)) {
        LOGI("%s: disabled — skipping start", s->name);
        return 0;
    }

    /* deferred: start-condition — if false, skip start (stay STOPPED).
     * Re-evaluated each call so the operator can `start X` again once the
     * condition becomes true (e.g. mount appeared). */
    if (s->start_cond.kind1 != HZ_SC_NONE && !evaluate_sc(&s->start_cond)) {
        LOGI("%s: start-condition false — skipping", s->name);
        return 0;
    }

    /* honor requires: start deps first (lazy: no topo, just recurse) */
    for (int i = 0; i < s->n_deps; i++) {
        hz_service_t *d = find_service(s->deps[i]);
        if (!d) {
            /* deferred: not a real service — check virtual intents
             * (network_ready). If true, treat dep as satisfied; if false,
             * refuse to start (this service would fail without networking).
             * If -1 (not virtual either), warn and continue — preserves
             * old behavior of tolerating forward references. */
            int v = virtual_dep_running(s->deps[i]);
            if (v == 1) continue;
            if (v == 0) {
                LOGE("%s: dep '%s' (virtual) not satisfied", s->name, s->deps[i]);
                s->state = HZ_S_FAILED;
                return -1;
            }
            LOGW("%s: unknown dep '%s'", s->name, s->deps[i]);
            continue;
        }
        if (d->state != HZ_S_RUNNING) {
            if (start_service(d) != 0) {
                LOGE("%s: dep %s failed to start", s->name, d->name);
                s->state = HZ_S_FAILED;
                return -1;
            }
        }
    }

    /* backoff: if restarted too many times recently, refuse */
    if (s->restart_count >= s->max_restarts && s->max_restarts > 0) {
        LOGE("%s: giving up after %d restarts", s->name, s->restart_count);
        mark_failed(s);
        return -1;
    }

    /* deferred: cgroup v2 — create per-service dir, write limits. No-op if
     * cgroup v2 isn't mounted (LOGW once at startup). */
    cgroup_setup_for(s);

    /* v2.2: pre-start hook — run a shell command before forking. Failure
     * (non-zero exit) aborts the start. deferred: no timeout on pre-start. */
    if (s->pre_start[0]) {
        int rc = system(s->pre_start);
        if (rc != 0) {
            LOGE("%s: pre-start '%s' exited %d — aborting start", s->name, s->pre_start, rc);
            mark_failed(s);
            return -1;
        }
        LOGI("%s: pre-start '%s' ok", s->name, s->pre_start);
    }

    pid_t pid = fork();
    if (pid < 0) { LOGE("fork %s: %s", s->name, strerror(errno)); mark_failed(s); return -1; }
    if (pid == 0) {
        /* child */
        setsid();
        /* deferred: parent blocked SIGCHLD/SIGTERM/SIGINT/SIGHUP for signalfd.
         * Reset to empty so children get default signal behavior. Without this,
         * `kill <service-pid>` does nothing until the service unblocks signals. */
        sigset_t empty; sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        /* v2.0: container integration (namespace + pivot_root + binds).
         * Failure here is logged but not fatal — degraded mode. */
        setup_container_child(s);
        /* v2.2: rootfs-readonly — remount the new root MS_RDONLY after pivot.
         * Only meaningful if rootfs: is set; no-op otherwise. */
        if (s->container.rootfs[0] && s->container.readonly) {
            mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
        }
        /* v2.2: no-new-privs — prctl before exec so the service (and its
         * children) can't gain caps via setuid binaries. */
        if (s->no_new_privs) {
            prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        }
        /* v2.2: per-service-user — setgid first (setuid would drop the
         * ability to setgid), then setuid, then clear supplementary groups. */
        if (s->run_as_gid) {
            if (setgid(s->run_as_gid) < 0) {
                LOGE("%s: setgid(%d): %s", s->name, s->run_as_gid, strerror(errno));
                _exit(127);
            }
        }
        if (s->run_as_uid) {
            if (setuid(s->run_as_uid) < 0) {
                LOGE("%s: setuid(%d): %s", s->name, s->run_as_uid, strerror(errno));
                _exit(127);
            }
        }
        /* v2.0: socket activation — dup listen fds to 3, 4, ... so the child
         * inherits them and can accept() directly. Set HZ_LISTEN_FDS env. */
        int nfd = 0;
        for (int i = 0; i < s->n_listen_fds; i++) {
            if (s->listen_fds[i] < 0) continue;
            int target = 3 + nfd;
            if (s->listen_fds[i] != target) {
                dup2(s->listen_fds[i], target);
            }
            nfd++;
        }
        /* deferred: per-service log redirect. log_path empty = inherit
         * hoshizora's stderr (goes to the system console). Set = open the
         * file O_APPEND so restarts don't clobber previous output. */
        if (s->log_path[0]) {
            int lfd = open(s->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (lfd >= 0) { dup2(lfd, 1); dup2(lfd, 2); if (lfd > 2) close(lfd); }
            /* else: open failed — leave stdout/stderr inherited. LOGE from
             * child would go to hoshizora's stderr; not done here to keep
             * the child silent post-fork (errors exit 127, parent logs). */
        }
        /* build argv: argv[0] = exec, then args */
        char *argv[HZ_MAX_ARGS + 2];
        argv[0] = s->exec;
        for (int i = 0; i < s->argc; i++) argv[i + 1] = s->argv[i];
        argv[s->argc + 1] = NULL;
        /* build envp: inherited env + service env + v2.0 extras.
         * deferred: no free() — child execs or _exit()s, leaks don't matter. */
        extern char **environ;
        char *envp[HZ_MAX_ENV + 64];
        int ei = 0;
        for (char **e = environ; *e && ei < 60; e++) envp[ei++] = *e;
        for (int i = 0; i < s->n_env && ei < HZ_MAX_ENV + 60; i++) {
            char buf[HZ_MAX_NAME + HZ_MAX_STR + 4];
            snprintf(buf, sizeof(buf), "%s=%s", s->env_keys[i], s->env_vals[i]);
            envp[ei++] = strdup(buf);
        }
        /* v2.0 extras: notify socket (if expect-notify) + listen-fd count (if any) */
        char ebuf[256 + 32];
        if (s->expect_notify) {
            snprintf(ebuf, sizeof(ebuf), "HZ_NOTIFY_SOCKET=%s", g_notify_path);
            envp[ei++] = strdup(ebuf);
        }
        if (nfd > 0) {
            snprintf(ebuf, sizeof(ebuf), "HZ_LISTEN_FDS=%d", nfd);
            envp[ei++] = strdup(ebuf);
        }
        envp[ei] = NULL;
        execve(s->exec, argv, envp);
        LOGE("execve %s: %s", s->exec, strerror(errno));
        _exit(127);
    }
    s->pid = pid;
    s->state = HZ_S_RUNNING;
    s->notify_ready = 0;  /* v2.0: clear on (re)start */
    s->watchdog_last = mono_now();  /* v2.2: arm watchdog baseline */
    /* v2.0: arm start_deadline if timeout-start is set. Disarmed by
     * run_health_checks when state != RUNNING past deadline, or by
     * handle_notify_event on READY=1. */
    if (s->timeout_start > 0) {
        s->start_deadline = mono_now() + s->timeout_start;
    }
    cgroup_assign_pid(s, pid);
    LOGI("started %s (pid=%d)%s%s", s->name, pid,
         s->expect_notify ? " (expect-notify)" : "",
         s->n_listen_fds > 0 ? " (listen-fds)" : "");
    event_broadcast("START service=%s pid=%d\n", s->name, pid);
    return 0;
}

static int stop_service(hz_service_t *s) {
    /* deferred: delegate to stop_parallel with a 1-element list. Old inline
     * SIGTERM→5s-wait→SIGKILL was a copy of stop_parallel's logic — one
     * implementation, two callers (here + shutdown_all/reload). */
    s->manual_stop = 1;  /* even if not running, mark so pending respawn cancels */
    s->respawn_at  = 0;
    if (s->state != HZ_S_RUNNING || s->pid <= 0) return 0;
    hz_service_t *list[1] = { s };
    stop_parallel(list, 1, "stop");
    /* v2.2: post-stop hook — fire after the service is reaped. Best-effort:
     * no timeout, no status check, just runs. deferred: log exit code. */
    if (s->post_stop[0]) {
        int rc = system(s->post_stop);
        LOGI("%s: post-stop '%s' exited %d", s->name, s->post_stop, rc);
    }
    return 0;
}

/* deferred: signals handled via signalfd in the main poll loop — see
 * setup_signalfd() and handle_signal() further down. No async-signal
 * handlers, no flag globals (except g_shutdown for the main loop test). */

static void reload_config(const char *path);

/* ---------------------------------------------------------------------------
 * CGROUPS v2 — per-service resource limits. If cgroup v2 isn't mounted at
 * /sys/fs/cgroup, all functions are no-ops and services run unconstrained.
 * deferred: mkdir per-service dir, write memory.max + cpu.weight, assign child
 * pid to cgroup.procs. Teardown rmdir's the dir after the process exits.
 * ------------------------------------------------------------------------- */
static int cgroup_v2_available(void) {
    struct statfs sb;
    if (statfs("/sys/fs/cgroup", &sb) != 0) return 0;
    return sb.f_type == HZ_CGROUP2_SUPER_MAGIC;
}

static void cgroup_setup_for(const hz_service_t *s) {
    if (s->memory_limit == 0 && s->cpu_weight == 0 && !s->oom_kill_group
        && s->io_weight == 0 && s->memory_high == 0 && s->cpu_max_quota == 0) return;
    if (!cgroup_v2_available()) {
        /* deferred: would log per-service-per-start; check at startup instead.
         * Logged once in main() right after setup_fanotify. */
        return;
    }
    char path[HZ_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", HZ_CGROUP_BASE, s->name);
    if (mkdir(HZ_CGROUP_BASE, 0755) != 0 && errno != EEXIST) {
        LOGW("%s: mkdir %s: %s", s->name, HZ_CGROUP_BASE, strerror(errno));
        return;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        LOGW("%s: mkdir cgroup %s: %s", s->name, path, strerror(errno));
        return;
    }
    char file[HZ_MAX_PATH + 32];
    /* v2.2 helper — write a cgroup file with an integer/string value. */
    #define HZ_CG_WRITE(field, fmt, val) do { \
        snprintf(file, sizeof(file), "%s/%s", path, field); \
        int fd_ = open(file, O_WRONLY); \
        if (fd_ < 0) LOGW("%s: open %s: %s", s->name, file, strerror(errno)); \
        else { dprintf(fd_, fmt, val); close(fd_); } \
    } while (0)
    if (s->memory_limit > 0) HZ_CG_WRITE("memory.max", "%llu", (unsigned long long)s->memory_limit);
    if (s->cpu_weight > 0)   HZ_CG_WRITE("cpu.weight", "%d", s->cpu_weight);
    if (s->oom_kill_group)   HZ_CG_WRITE("memory.oom.group", "%d", 1);
    /* v2.2: new cgroup fields */
    if (s->io_weight > 0)    HZ_CG_WRITE("io.weight", "%d", s->io_weight);
    if (s->memory_high > 0)  HZ_CG_WRITE("memory.high", "%llu", (unsigned long long)s->memory_high);
    if (s->cpu_max_quota > 0) {
        char buf[64];
        int period = s->cpu_max_period > 0 ? s->cpu_max_period : 100000;
        snprintf(buf, sizeof(buf), "%s/%s", path, "cpu.max");
        int fd_ = open(buf, O_WRONLY);
        if (fd_ < 0) LOGW("%s: open %s: %s", s->name, buf, strerror(errno));
        else { dprintf(fd_, "%d %d", s->cpu_max_quota, period); close(fd_); }
    }
    #undef HZ_CG_WRITE
    LOGI("%s: cgroup %s (mem=%lluB, cpu=%d%s)", s->name, path,
         (unsigned long long)s->memory_limit, s->cpu_weight,
         s->oom_kill_group ? ", oom-group" : "");
}

static void cgroup_assign_pid(const hz_service_t *s, pid_t pid) {
    if (s->memory_limit == 0 && s->cpu_weight == 0 && !s->oom_kill_group) return;
    char file[HZ_MAX_PATH + 32];
    snprintf(file, sizeof(file), "%s/%s/cgroup.procs", HZ_CGROUP_BASE, s->name);
    int fd = open(file, O_WRONLY);
    if (fd < 0) { LOGW("%s: open %s: %s", s->name, file, strerror(errno)); return; }
    dprintf(fd, "%d", (int)pid);
    close(fd);
}

static void cgroup_teardown_for(const hz_service_t *s) {
    if (s->memory_limit == 0 && s->cpu_weight == 0 && !s->oom_kill_group) return;
    char path[HZ_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", HZ_CGROUP_BASE, s->name);
    if (rmdir(path) != 0 && errno != ENOENT) {
        LOGW("%s: rmdir cgroup %s: %s", s->name, path, strerror(errno));
    }
}

/* ---------------------------------------------------------------------------
 * MAIN LOOP
 * ------------------------------------------------------------------------- */
static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < g_sys.n_services; i++) {
            hz_service_t *s = &g_sys.services[i];
            if (s->pid != pid) continue;
            int crashed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
            LOGI("%s (pid=%d) exited status=%d crashed=%d",
                 s->name, pid, status, crashed);
            event_broadcast("EXIT service=%s pid=%d status=%d crashed=%d\n",
                            s->name, pid, status, crashed);
            s->pid = 0;
            if (g_sys.shutting_down) { s->state = HZ_S_STOPPED; break; }
            /* deferred: exit 127 = execve failed (ENOENT/EACCES/etc). Respawning
             * would hit the same error instantly, max_restarts times, before
             * giving up. Mark FAILED and let the operator fix the config. */
            if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                LOGE("%s: exec failed (exit 127) — not respawning", s->name);
                /* v2.0: if the service has a start-condition, the failure may
                 * be transient (fs not mounted yet). Re-arm cron_next with
                 * retry_after (default 5s) so the operator can retry. Up to
                 * max_restarts retries; then mark_failed. */
                if (s->start_cond.kind1 != HZ_SC_NONE && s->restart_count < s->max_restarts) {
                    int delay = s->retry_after > 0 ? s->retry_after : 5;
                    s->restart_count++;
                    s->state = HZ_S_STOPPED;
                    s->cron_next = mono_now() + delay;
                    LOGI("%s: exec failed, retrying in %ds (attempt %d)",
                         s->name, delay, s->restart_count);
                    break;
                }
                mark_failed(s);
                s->respawn_at = 0;
                break;
            }
            if (crashed && s->respawn && !s->manual_stop) {
                s->restart_count++;
                /* deferred: state → STOPPED so start_service will accept the
                 * respawn. Without this, state stays RUNNING from before the
                 * crash and start_service's `if (state == RUNNING) return 0`
                 * short-circuits, blocking respawn forever. */
                s->state = HZ_S_STOPPED;
                /* deferred: linear backoff = restart_count seconds, capped at 30.
                 * Non-blocking — scheduled via respawn_at, fired by main loop's
                 * timerfd. Old blocking sleep() would freeze the control socket. */
                int delay = s->restart_count;
                if (delay > 30) delay = 30;
                s->respawn_at = mono_now() + delay;
                LOGI("%s: respawn scheduled in %ds (attempt %d)",
                     s->name, delay, s->restart_count);
            } else if (!crashed || s->manual_stop) {
                s->state = HZ_S_STOPPED;
                if (!crashed) s->restart_count = 0;
                /* deferred: cron job finished cleanly — re-arm next fire.
                 * Crashed cron jobs fall through to the FAILED/respawn path
                 * below; they don't auto-re-arm. */
                if (!crashed && s->cron_interval > 0 && !s->manual_stop) {
                    s->cron_next = mono_now() + s->cron_interval;
                    LOGI("%s: cron re-armed, next fire in %ds",
                         s->name, s->cron_interval);
                }
            } else {
                mark_failed(s);
            }
            break;
        }
    }
}

/* deferred: parallel stop — SIGTERM all, wait once (5s), SIGKILL stragglers.
 * Used by both shutdown_all (all services, reverse order) and reload (changed
 * services, list order). Single implementation — old serial path called
 * stop_service per service, each blocking up to 5s → 64 services = 320s of
 * unresponsive PID 1. Parallel is O(5s) total. */
static void stop_parallel(hz_service_t **list, int n, const char *why) {
    /* pass 1: SIGTERM all */
    for (int k = 0; k < n; k++) {
        hz_service_t *s = list[k];
        s->manual_stop = 1;
        s->respawn_at = 0;
        if (s->state == HZ_S_RUNNING && s->pid > 0) {
            LOGI("%s: stopping %s (pid=%d)", why, s->name, s->pid);
            kill(s->pid, SIGTERM);
        }
    }
    /* pass 2: poll waitpid up to 5s for all to exit */
    for (int tick = 0; tick < 50; tick++) {
        int alive = 0;
        for (int k = 0; k < n; k++) {
            hz_service_t *s = list[k];
            if (s->pid > 0) {
                pid_t r = waitpid(s->pid, NULL, WNOHANG);
                if (r == s->pid || r < 0) {
                    s->pid = 0; s->state = HZ_S_STOPPED;
                    cgroup_teardown_for(s);
                } else alive++;
            }
        }
        if (alive == 0) break;
        usleep(100000);
    }
    /* pass 3: SIGKILL stragglers, blocking wait */
    for (int k = 0; k < n; k++) {
        hz_service_t *s = list[k];
        if (s->pid > 0) {
            LOGW("%s: %s didn't exit on SIGTERM, SIGKILL", why, s->name);
            kill(s->pid, SIGKILL);
            waitpid(s->pid, NULL, 0);
            s->pid = 0; s->state = HZ_S_STOPPED;
            cgroup_teardown_for(s);
        }
    }
}

/* deferred: shutdown_all = build reverse list of all services, call stop_parallel. */
static void shutdown_all(void) {
    g_sys.shutting_down = 1;
    LOGI("shutdown: stopping %d services", g_sys.n_services);
    event_broadcast("SHUTDOWN services=%d\n", g_sys.n_services);
    hz_service_t *list[HZ_MAX_SERVICES];
    int n = 0;
    for (int i = g_sys.n_services - 1; i >= 0; i--)
        list[n++] = &g_sys.services[i];
    stop_parallel(list, n, "shutdown");
}

/* ---------------------------------------------------------------------------
 * SIGNAFD + CONTROL SOCKET + TIMERFD setup
 * ------------------------------------------------------------------------- */

/* deferred: signalfd integrates signals into the poll loop — no async-signal
 * handlers, no self-pipe. Block the signals first so they queue for the fd.
 * SFD_NONBLOCK so handle_signal's drain loop doesn't block when empty. */
static int setup_signalfd(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1;
    g_sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    /* SIGPIPE still ignored via SIG_IGN — services that write to closed
     * pipes should crash and respawn rather than take PID 1 with them. */
    signal(SIGPIPE, SIG_IGN);
    return g_sigfd;
}

static int setup_control_socket(void) {
    /* deferred: env var override for tests; default /run/hoshizora/control */
    const char *env = getenv("HZ_CTL_PATH");
    if (env && *env) {
        strncpy(g_ctl_path, env, sizeof(g_ctl_path) - 1);
        g_ctl_path[sizeof(g_ctl_path) - 1] = 0;
    }
    /* sun_path is 108 bytes; longer paths would silently truncate and bind at
     * the wrong path. Refuse loudly instead. */
    if (strlen(g_ctl_path) >= 108) {
        LOGE("control socket path too long (max 107 chars): %s", g_ctl_path);
        return -1;
    }
    /* mkdir -p parent dir of g_ctl_path */
    mkdir_parents_of(g_ctl_path);
    unlink(g_ctl_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_ctl_path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    /* deferred: 0660 — root-only by default. Add a hoshizora group + chgrp
     * when non-root operators need access. */
    chmod(g_ctl_path, 0660);
    listen(fd, 8);
    g_ctlfd = fd;
    return fd;
}

static int setup_reload_timer(void) {
    /* deferred: CLOCK_MONOTONIC — immune to NTP step adjustments and avoids
     * the integer-second truncation bug that CLOCK_REALTIME + TFD_TIMER_ABSTIME
     * had with respawn_at <= now checks (timer fires at X.5s, time(NULL)
     * returns X, respawn_at = X+1 → never respawns, busy-loops). */
    g_reloadfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    return g_reloadfd;
}

/* deferred: monotonic clock in seconds — used for respawn_at.
 * Wall clock (time(NULL)) is wrong here: NTP can step it backward, and
 * integer-second truncation breaks absolute-time timer comparisons. */
static time_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static int setup_health_timer(void) {
    /* deferred: separate timerfd fired every 5s for health probes. Reuses the
     * interval mode of timerfd (it_value = first fire, it_interval = repeat). */
    g_healthfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (g_healthfd < 0) return -1;
    struct itimerspec its = {0};
    its.it_value.tv_sec    = 5;
    its.it_interval.tv_sec = 5;
    timerfd_settime(g_healthfd, 0, &its, NULL);
    return 0;
}

static void run_health_checks(void) {
    time_t now = mono_now();
    /* v2.0: check start_deadline for services that didn't reach RUNNING
     * (or didn't send READY=1) within timeout-start. 5s tick granularity. */
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *s = &g_sys.services[i];
        if (s->start_deadline == 0) continue;
        if (s->state != HZ_S_RUNNING) continue;  /* only running services can miss the deadline */
        if (s->expect_notify && !s->notify_ready) {
            /* still waiting for READY=1 */
            if (now >= s->start_deadline) {
                LOGE("%s: timeout-start expired (no READY=1) — marking FAILED", s->name);
                s->start_deadline = 0;
                kill(s->pid, SIGTERM);
                mark_failed(s);
            }
            continue;
        }
        /* no expect-notify: deadline disarms implicitly once RUNNING.
         * If we reach here with start_deadline set, the service is RUNNING
         * and not expecting notify → disarm. */
        s->start_deadline = 0;
    }
    /* v2.2: watchdog — services with watchdog_timeout set must send
     * WATCHDOG=1 within that interval or be marked FAILED. */
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *s = &g_sys.services[i];
        if (s->watchdog_timeout <= 0 || s->state != HZ_S_RUNNING) continue;
        if (now - s->watchdog_last > s->watchdog_timeout) {
            LOGE("%s: watchdog timeout (no WATCHDOG=1 in %ds) — marking FAILED",
                 s->name, s->watchdog_timeout);
            kill(s->pid, SIGTERM);
            mark_failed(s);
        }
    }
    /* deferred: probe every RUNNING service with health.hostport set.
     * 3 consecutive failures → mark FAILED + SIGTERM. Probes are sequential
     * (max 64 services × 5s timeout = 320s worst case — but real services
     * answer in <10ms, and timeouts are rare; acceptable for PID 1). */
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *s = &g_sys.services[i];
        if (s->state != HZ_S_RUNNING || s->pid <= 0) continue;
        if (s->health.hostport[0] == 0) continue;
        if (tcp_probe(s->health.hostport, s->health.timeout_s) == 0) {
            if (s->health.fail_count > 0) {
                LOGI("%s: health probe ok — resetting fail count", s->name);
            }
            s->health.fail_count = 0;
        } else {
            s->health.fail_count++;
            LOGW("%s: health probe failed (%d/3)", s->name, s->health.fail_count);
            if (s->health.fail_count >= 3) {
                LOGE("%s: unhealthy — marking FAILED", s->name);
                kill(s->pid, SIGTERM);
                /* reap_children will see the exit; respawn logic kicks in if
                 * the service is configured for it. manual_stop stays 0 so
                 * respawn isn't blocked. */
                s->health.fail_count = 0;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * FANOTIFY — file watches. Single fd, mark all paths, longest-prefix-match
 * events to watches. deferred: needs CAP_SYS_ADMIN. If init isn't running as
 * root (test mode), fanotify_init fails and watches are inert — services still
 * run, just no auto-reload/restart on file changes.
 * ------------------------------------------------------------------------- */
static int setup_fanotify(void) {
    if (g_sys.n_watches == 0) return 0;  /* nothing to watch */
    g_fanfd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK,
                            O_RDONLY | O_CLOEXEC);
    if (g_fanfd < 0) {
        LOGW("fanotify_init: %s — file watches disabled", strerror(errno));
        g_fanfd = -1;
        return -1;
    }
    unsigned mask = FAN_MODIFY | FAN_CLOSE_WRITE | FAN_MOVED_FROM |
                    FAN_MOVED_TO | FAN_CREATE | FAN_DELETE;
    int marked = 0;
    for (int i = 0; i < g_sys.n_watches; i++) {
        hz_watch_t *w = &g_sys.watches[i];
        /* v2.2: recursive keyword honored via FAN_MARK_FILESYSTEM (kernel ≥5.16).
         * Marks the whole filesystem containing w->path. deferred: per-subtree
         * filtering — operator gets events for the whole fs, plugin can grep. */
        unsigned flags = FAN_MARK_ADD;
        if (w->recursive) flags |= FAN_MARK_FILESYSTEM;
        if (fanotify_mark(g_fanfd, flags, mask, AT_FDCWD, w->path) < 0) {
            /* FAN_MARK_FILESYSTEM may be unsupported on older kernels — retry
             * without it so we at least watch the top dir (pre-v2.2 behavior). */
            if (w->recursive && errno == EINVAL) {
                LOGW("fanotify_mark %s: FAN_MARK_FILESYSTEM unsupported — top-dir only", w->path);
                if (fanotify_mark(g_fanfd, FAN_MARK_ADD, mask, AT_FDCWD, w->path) < 0) {
                    LOGW("fanotify_mark %s: %s — skipping", w->path, strerror(errno));
                    continue;
                }
            } else {
                LOGW("fanotify_mark %s: %s — skipping", w->path, strerror(errno));
                continue;
            }
        }
        marked++;
        LOGI("watching %s -> %s %s", w->path,
             w->action == HZ_W_RELOAD ? "reload" : "restart",
             w->service);
    }
    if (marked == 0) {
        LOGW("fanotify: no paths marked successfully — closing fd");
        close(g_fanfd); g_fanfd = -1;
        return -1;
    }
    return 0;
}

/* on fanotify event: readlink /proc/self/fd/<event-fd> to get path,
 * longest-prefix-match against watches, dispatch reload/restart. */
static void handle_fanotify_event(void) {
    char buf[8192];
    ssize_t n = read(g_fanfd, buf, sizeof(buf));
    if (n <= 0) return;
    /* deferred: walk fixed-size event metadata records */
    for (char *p = buf; p + sizeof(struct fanotify_event_metadata) <= buf + n;
         p += ((struct fanotify_event_metadata*)p)->event_len) {
        struct fanotify_event_metadata *e = (struct fanotify_event_metadata*)p;
        if (e->fd < 0) continue;
        /* resolve path */
        char proc[64], path[HZ_MAX_PATH];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", e->fd);
        ssize_t pl = readlink(proc, path, sizeof(path) - 1);
        close(e->fd);
        if (pl < 0) continue;
        path[pl] = 0;
        /* longest-prefix match */
        hz_watch_t *best = NULL; size_t best_len = 0;
        for (int i = 0; i < g_sys.n_watches; i++) {
            hz_watch_t *w = &g_sys.watches[i];
            size_t wlen = strlen(w->path);
            if (strncmp(path, w->path, wlen) == 0 && wlen > best_len) {
                best = w; best_len = wlen;
            }
        }
        if (!best) continue;
        hz_service_t *s = find_service(best->service);
        if (!s) { LOGW("watch %s: target service %s not found", best->path, best->service); continue; }
        LOGI("watch %s: change in %s -> %s %s", best->path, path,
             best->action == HZ_W_RELOAD ? "reload" : "restart", best->service);
        if (best->action == HZ_W_RELOAD) {
            /* send SIGHUP to the service — let it reload itself */
            if (s->pid > 0) kill(s->pid, SIGHUP);
        } else {
            /* restart: stop (sets manual_stop) + start (clears manual_stop) */
            stop_service(s);
            start_service(s);
        }
    }
}

/* ---------------------------------------------------------------------------
 * CONTROL SOCKET PROTOCOL
 * deferred: text protocol, one connection per command, single-line response
 * (except `list` which emits one line per service). No framing, no JSON.
 *
 * Commands:
 *   list                       — list all services with state
 *   status [name]              — same as list (or one service)
 *   start <name>               — start a stopped/manual-stopped service
 *   stop <name>                — stop a running service (no respawn)
 *   restart <name>             — stop + start
 *   reload                     — re-read config, diff + apply
 *   shutdown                   — stop all + exit
 * ------------------------------------------------------------------------- */

static void ctl_send(int fd, const char *s) {
    (void)write(fd, s, strlen(s));
    (void)write(fd, "\n", 1);
}

static const char *state_str(hz_state_t st) {
    switch (st) {
    case HZ_S_STOPPED:  return "stopped";
    case HZ_S_RUNNING:  return "running";
    case HZ_S_FAILED:   return "failed";
    }
    return "?";
}

static void handle_control_client(int cfd) {
    char buf[1024];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(cfd); return; }
    buf[n] = 0;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    char *cmd = buf;
    char *arg = strchr(buf, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }

    /* v2.3: SOV-only protocol. <name> <action> [<args>]. The first word is
     * either a known top-level command (no subject — list/show/logs/etc.)
     * OR a service name (subject), with the second word being the action
     * (verb). Action-first form (`start nginx`) is no longer auto-reordered;
     * if a user types it, `start` is treated as a service name and the
     * command fails with "no such service". deferred: a wrapper script
     * (hzctl-systemctl) can translate action-first if anyone needs it. */
    char *name = NULL;     /* subject (service name or target name) */
    char *action = cmd;    /* verb (or first word if no subject) */
    char *action_arg = arg;/* optional 3rd word (e.g. logs N, reload X) */

    /* If cmd isn't a known top-level command, treat it as a subject and
     * pull the action from the second word. */
    static const char *top[] = {
        "list", "status", "show", "logs", "daemon-reload", "reload",
        "shutdown", "poweroff", "reboot", "help", "?", NULL
    };
    int is_top = 0;
    for (int i = 0; top[i]; i++) if (strcmp(cmd, top[i]) == 0) { is_top = 1; break; }
    if (!is_top && arg && *arg) {
        name = cmd;
        action = arg;
        action_arg = strchr(action, ' ');
        if (action_arg) { *action_arg++ = 0; while (*action_arg == ' ') action_arg++; }
        else action_arg = NULL;
    }

    /* v2.3: SOV dispatch. `action` is the verb; `name` is the subject
     * (service or target, NULL for self-only commands); `action_arg` is
     * the optional object (e.g. `logs N`). */
    if (strcmp(action, "list") == 0 || strcmp(action, "status") == 0) {
        char line[HZ_MAX_NAME + 160];
        if (name && *name) {
            hz_service_t *s = find_service(name);
            if (!s) { ctl_send(cfd, "no such service"); close(cfd); return; }
            snprintf(line, sizeof(line), "%-20s %-10s pid=%-6d restarts=%d%s%s",
                     s->name, state_str(s->state), (int)s->pid, s->restart_count,
                     service_enabled(s) ? "" : " [disabled]",
                     (s->start_cond.kind1 != HZ_SC_NONE && !evaluate_sc(&s->start_cond))
                        ? " [cond-false]" : "");
            ctl_send(cfd, line);
        } else {
            for (int i = 0; i < g_sys.n_services; i++) {
                hz_service_t *s = &g_sys.services[i];
                snprintf(line, sizeof(line), "%-20s %-10s pid=%-6d restarts=%d%s%s",
                         s->name, state_str(s->state), (int)s->pid, s->restart_count,
                         service_enabled(s) ? "" : " [disabled]",
                         (s->start_cond.kind1 != HZ_SC_NONE && !evaluate_sc(&s->start_cond))
                            ? " [cond-false]" : "");
                ctl_send(cfd, line);
            }
            if (g_sys.n_services == 0) ctl_send(cfd, "(no services)");
        }
    } else if (strcmp(action, "show") == 0) {
        char line[HZ_MAX_NAME + 64];
        for (int i = 0; i < g_sys.n_services; i++) {
            hz_service_t *s = &g_sys.services[i];
            snprintf(line, sizeof(line), "%-20s %s",
                     s->name, service_enabled(s) ? "enabled" : "disabled");
            ctl_send(cfd, line);
        }
        if (g_sys.n_services == 0) ctl_send(cfd, "(no services)");
    } else if (strcmp(action, "start") == 0 && name) {
        hz_service_t *s = find_service(name);
        if (s) { start_service(s); ctl_send(cfd, "ok"); }
        else {
            /* v2.0: target — start all services in the named target. */
            hz_target_t *tg = find_target(name);
            if (tg) {
                int started = 0;
                for (int i = 0; i < tg->n_services; i++) {
                    hz_service_t *ts = find_service(tg->services[i]);
                    if (ts) { start_service(ts); started++; }
                }
                char msg[64];
                snprintf(msg, sizeof(msg), "ok (target %s: %d services)", tg->name, started);
                ctl_send(cfd, msg);
            } else {
                ctl_send(cfd, "no such service or target");
            }
        }
    } else if (strcmp(action, "stop") == 0 && name) {
        hz_service_t *s = find_service(name);
        if (!s) ctl_send(cfd, "no such service");
        else { stop_service(s); ctl_send(cfd, "ok"); }
    } else if (strcmp(action, "restart") == 0 && name) {
        hz_service_t *s = find_service(name);
        if (!s) ctl_send(cfd, "no such service");
        else { stop_service(s); start_service(s); ctl_send(cfd, "ok"); }
    } else if (strcmp(action, "reload") == 0) {
        /* `<name> reload` = per-service SIGHUP; `reload` (no name) = daemon-reload. */
        if (name && *name) {
            hz_service_t *s = find_service(name);
            if (!s) ctl_send(cfd, "no such service");
            else if (s->pid > 0) { kill(s->pid, SIGHUP); ctl_send(cfd, "ok"); }
            else ctl_send(cfd, "not running");
        } else {
            reload_config(g_cfg_path);
            ctl_send(cfd, "ok");
        }
    } else if (strcmp(action, "daemon-reload") == 0) {
        reload_config(g_cfg_path);
        ctl_send(cfd, "ok");
    } else if (strcmp(action, "enable") == 0 && name) {
        char p[HZ_MAX_PATH];
        enabled_path_for(name, p, sizeof(p));
        if (unlink(p) != 0 && errno != ENOENT) ctl_send(cfd, "error");
        else ctl_send(cfd, "ok");
    } else if (strcmp(action, "disable") == 0 && name) {
        char p[HZ_MAX_PATH];
        enabled_path_for(name, p, sizeof(p));
        mkdir_parents_of(p);  /* creates <ctl-dir>/ and <ctl-dir>/enabled/ */
        int fd2 = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd2 < 0) ctl_send(cfd, "error");
        else { close(fd2); ctl_send(cfd, "ok"); }
    } else if (strcmp(action, "logs") == 0) {
        int lines = (action_arg && *action_arg) ? atoi(action_arg) : 50;
        logs_dump(cfd, lines);
    } else if (strcmp(action, "shutdown") == 0 || strcmp(action, "poweroff") == 0) {
        g_reboot_target = 0; g_shutdown = 1;
        ctl_send(cfd, "powering off");
    } else if (strcmp(action, "reboot") == 0) {
        g_reboot_target = 1; g_shutdown = 1;
        ctl_send(cfd, "rebooting");
    } else if (strcmp(action, "help") == 0 || strcmp(action, "?") == 0) {
        ctl_send(cfd, "commands (SOV: <name> <action> [<arg>]):");
        ctl_send(cfd, "  <name> start | stop | restart");
        ctl_send(cfd, "  <name> reload                    per-service SIGHUP");
        ctl_send(cfd, "  <name> status | enable | disable");
        ctl_send(cfd, "  list                             list all services + state");
        ctl_send(cfd, "  status [<name>]                  status of one or all");
        ctl_send(cfd, "  show                             list services + enabled state");
        ctl_send(cfd, "  reload                           daemon-reload (re-read config)");
        ctl_send(cfd, "  daemon-reload                    explicit alias for above");
        ctl_send(cfd, "  logs [N]                         last N log lines (default 50)");
        ctl_send(cfd, "  shutdown | poweroff              sync + power off");
        ctl_send(cfd, "  reboot                           sync + reboot");
        ctl_send(cfd, "  <target> start                   start all services in target");
    } else {
        ctl_send(cfd, "unknown command (try 'help')");
    }
    close(cfd);
}

/* ---------------------------------------------------------------------------
 * RELOAD — re-read config, diff, apply.
 * deferred: snapshot running pids by name, re-read, walk new config to adopt
 * or restart, walk snapshot to kill removed. ~30 lines, no clever diffing.
 * ------------------------------------------------------------------------- */

static int service_spec_changed(const hz_service_t *a, const hz_service_t *b) {
    if (strcmp(a->exec, b->exec) != 0) return 1;
    if (a->argc != b->argc) return 1;
    for (int i = 0; i < a->argc; i++) if (strcmp(a->argv[i], b->argv[i]) != 0) return 1;
    if (a->n_env != b->n_env) return 1;
    for (int i = 0; i < a->n_env; i++) {
        if (strcmp(a->env_keys[i], b->env_keys[i]) != 0) return 1;
        if (strcmp(a->env_vals[i], b->env_vals[i]) != 0) return 1;
    }
    if (a->respawn != b->respawn) return 1;
    if (a->n_deps != b->n_deps) return 1;
    for (int i = 0; i < a->n_deps; i++) if (strcmp(a->deps[i], b->deps[i]) != 0) return 1;
    /* deferred: spec changes that affect the running process — restart on diff.
     * memory/cpu/oom-kill/log_path/on-fail are start-time effects: cgroup
     * setup, log redirect, on-fail target wiring. start-cond/healthy are NOT
     * compared — they're re-evaluated at every start / every 5s probe, so a
     * config change takes effect without a restart. */
    if (a->memory_limit   != b->memory_limit)   return 1;
    if (a->cpu_weight     != b->cpu_weight)     return 1;
    if (a->oom_kill_group != b->oom_kill_group) return 1;
    if (strcmp(a->log_path, b->log_path) != 0)  return 1;
    /* on-fail: a runtime-side-effect change — restart picks up the new target */
    if (a->on_fail.kind != b->on_fail.kind)     return 1;
    if (a->on_fail.kind == HZ_ON_FAIL_RESTART &&
        strcmp(a->on_fail.target, b->on_fail.target) != 0) return 1;
    return 0;
}

static void reload_config(const char *path) {
    LOGI("reload: re-reading %s", path);
    /* deferred: snapshot pre-reload services (full structs) — used for both
     * spec-change detection AND runtime-field adoption. Old code had snap[]
     * (subset of fields) AND old[] (full structs) for the same purpose —
     * snap[] dropped, old[] serves both. */
    int old_n = g_sys.n_services;
    hz_service_t *old = malloc(sizeof(hz_service_t) * old_n);
    if (old) memcpy(old, g_sys.services, sizeof(hz_service_t) * old_n);
    else { LOGE("reload: out of memory — aborting"); return; }

    g_sys.n_services = 0;
    if (load_config(path) != 0) {
        LOGE("reload: parse failed, restoring old config");
        memcpy(g_sys.services, old, sizeof(hz_service_t) * old_n);
        g_sys.n_services = old_n;
        free(old);
        return;
    }

    /* walk new config: collect changed services for parallel stop, adopt
     * unchanged running pids. Ponytail: parallel stop keeps reload O(5s) total
     * even when many services changed, instead of O(N×5s) serial. */
    hz_service_t *to_stop[HZ_MAX_SERVICES];
    int n_to_stop = 0;
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *n = &g_sys.services[i];
        for (int j = 0; j < old_n; j++) {
            if (strcmp(n->name, old[j].name) != 0) continue;
            if (service_spec_changed(n, &old[j])) {
                LOGI("reload: %s spec changed — restarting", n->name);
                n->pid = old[j].pid;
                n->state = old[j].state;
                n->manual_stop = 1;  /* stop won't respawn */
                to_stop[n_to_stop++] = n;
            } else {
                /* adopt running pid, preserve runtime state */
                n->pid = old[j].pid;
                n->state = old[j].state;
                n->restart_count = old[j].restart_count;
                n->manual_stop = old[j].manual_stop;
                LOGI("reload: %s unchanged — adopted (pid=%d)", n->name, (int)n->pid);
            }
            break;
        }
    }

    /* parallel-stop all changed services in one 5s window */
    if (n_to_stop > 0) stop_parallel(to_stop, n_to_stop, "reload");
    /* clear manual_stop so the start pass below can pick them up */
    for (int k = 0; k < n_to_stop; k++) to_stop[k]->manual_stop = 0;

    /* walk old snapshot: kill pids for services no longer in config */
    for (int j = 0; j < old_n; j++) {
        if (!find_service(old[j].name)) {
            if (old[j].pid > 0) {
                LOGI("reload: %s removed from config — killing pid=%d",
                     old[j].name, (int)old[j].pid);
                kill(old[j].pid, SIGTERM);
            }
        }
    }

    /* start any service in STOPPED state that isn't manual_stop */
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *n = &g_sys.services[i];
        if (n->state == HZ_S_STOPPED && !n->manual_stop) {
            LOGI("reload: %s starting (new or restarted)", n->name);
            start_service(n);
        }
    }

    free(old);
    LOGI("reload: done (%d services)", g_sys.n_services);
}

/* ---------------------------------------------------------------------------
 * RESPAWN SCHEDULER — single timerfd armed to the next pending respawn_at
 * ------------------------------------------------------------------------- */

static void arm_respawn_timer(void) {
    time_t now = mono_now();
    time_t earliest = 0;
    for (int i = 0; i < g_sys.n_services; i++) {
        time_t r = g_sys.services[i].respawn_at;
        /* deferred: cron_next reuses the same timerfd — a cron job firing is
         * just another "wake me at time T" pending event. One timer, two
         * reasons to fire. */
        if (g_sys.services[i].cron_next) r = r ? (r < g_sys.services[i].cron_next ? r : g_sys.services[i].cron_next) : g_sys.services[i].cron_next;
        if (r == 0) continue;
        if (r <= now) { earliest = now; break; }
        if (earliest == 0 || r < earliest) earliest = r;
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (earliest == 0) {
        /* disarm */
        timerfd_settime(g_reloadfd, 0, &its, NULL);
        return;
    }
    its.it_value.tv_sec = earliest;
    its.it_value.tv_nsec = 0;
    timerfd_settime(g_reloadfd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void fire_due_respawns(void) {
    time_t now = mono_now();
    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *s = &g_sys.services[i];
        if (s->respawn_at != 0 && s->respawn_at <= now) {
            s->respawn_at = 0;
            if (!s->manual_stop) start_service(s);
        }
        /* deferred: cron fire — start_service sets state=RUNNING; reap_children
         * sees the clean exit, re-arms cron_next = now + cron_interval. Cron
         * jobs don't use respawn_at — they're not crashes. */
        if (s->cron_interval > 0 && s->cron_next != 0 && s->cron_next <= now
            && s->state != HZ_S_RUNNING && !s->manual_stop) {
            s->cron_next = 0;
            LOGI("%s: cron firing", s->name);
            start_service(s);
        }
    }
}

/* ---------------------------------------------------------------------------
 * MAIN LOOP — single poll() over signalfd, control socket, respawn timer
 * ------------------------------------------------------------------------- */

static void handle_signal(void) {
    struct signalfd_siginfo si;
    while (read(g_sigfd, &si, sizeof(si)) == sizeof(si)) {
        switch (si.ssi_signo) {
        case SIGCHLD: reap_children(); break;
        case SIGHUP:  reload_config(g_cfg_path); break;
        case SIGTERM:
        case SIGINT:  g_shutdown = 1; break;
        }
    }
}

int main(int argc, char **argv) {
    const char *cfg = (argc > 1) ? argv[1] : "/etc/hoshizora/system.hs";
    if (access(cfg, R_OK) != 0) {
        if (access("system.hs", R_OK) == 0) cfg = "system.hs";
        else { LOGE("no config at %s or ./system.hs", cfg); return 1; }
    }
    g_cfg_path = cfg;

    LOGI("hoshizora starting, config=%s", cfg);
    if (load_config(cfg) != 0) return 1;
    LOGI("loaded %d services, %d watches", g_sys.n_services, g_sys.n_watches);

    if (setup_signalfd() < 0)    { LOGE("signalfd: %s", strerror(errno)); return 1; }
    if (setup_control_socket() < 0) { LOGE("control socket: %s", strerror(errno)); return 1; }
    if (setup_reload_timer() < 0) { LOGE("timerfd: %s", strerror(errno)); return 1; }
    if (setup_health_timer() < 0) { LOGW("health timerfd: %s — health probes disabled", strerror(errno)); }
    setup_fanotify();  /* non-fatal if it fails — watches just become inert */
    setup_listen_sockets();  /* v2.0: bind socket-activation fds before services start */
    setup_notify_socket();   /* v2.0: sd_notify socket — non-fatal if it fails */
    setup_event_socket();    /* v2.1: plugin event broadcast — non-fatal if it fails */
    openlog("hoshizora", LOG_PID | LOG_NDELAY, LOG_DAEMON);  /* v2.0: journal integration */
    drop_capabilities();     /* v2.0: drop caps we don't need (warns if it fails) */

    /* deferred: one-time warning if cgroup v2 isn't available but config asks
     * for memory/cpu limits. Without this, limits silently don't apply. */
    if (!cgroup_v2_available()) {
        for (int i = 0; i < g_sys.n_services; i++) {
            if (g_sys.services[i].memory_limit > 0 || g_sys.services[i].cpu_weight > 0
                || g_sys.services[i].oom_kill_group) {
                LOGW("cgroup v2 not mounted at /sys/fs/cgroup — memory/cpu/oom limits will not be enforced");
                break;
            }
        }
    }
    LOGI("control socket: %s", g_ctl_path);

    for (int i = 0; i < g_sys.n_services; i++) {
        hz_service_t *s = &g_sys.services[i];
        /* deferred: cron jobs don't auto-start — they schedule their first fire
         * via cron_next, the timer fires start_service when due. */
        if (s->cron_interval > 0) {
            s->cron_next = mono_now() + s->cron_interval;
            LOGI("%s: cron scheduled, first fire in %ds", s->name, s->cron_interval);
            continue;
        }
        start_service(s);
    }

    struct pollfd pfds[7];
    pfds[0].fd = g_sigfd;    pfds[0].events = POLLIN;
    pfds[1].fd = g_ctlfd;    pfds[1].events = POLLIN;
    pfds[2].fd = g_reloadfd; pfds[2].events = POLLIN;
    pfds[3].fd = g_healthfd; pfds[3].events = POLLIN;
    pfds[4].fd = g_fanfd;    pfds[4].events = POLLIN;
    pfds[5].fd = g_notifyfd; pfds[5].events = POLLIN;  /* v2.0: sd_notify */
    pfds[6].fd = g_eventfd;  pfds[6].events = POLLIN;  /* v2.1: plugin subs */
    nfds_t npfds = 7;
    if (g_fanfd < 0)    pfds[4].fd = -1;
    if (g_notifyfd < 0) pfds[5].fd = -1;
    if (g_eventfd < 0)  pfds[6].fd = -1;

    while (!g_shutdown) {
        arm_respawn_timer();
        int r = poll(pfds, npfds, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOGE("poll: %s", strerror(errno));
            break;
        }
        if (pfds[0].revents & POLLIN) handle_signal();
        if (pfds[1].revents & POLLIN) {
            int cfd = accept4(g_ctlfd, NULL, NULL, SOCK_CLOEXEC);
            if (cfd >= 0) handle_control_client(cfd);
        }
        if (pfds[2].revents & POLLIN) {
            uint64_t exp;
            (void)read(g_reloadfd, &exp, sizeof(exp));
            fire_due_respawns();
        }
        if (g_healthfd >= 0 && (pfds[3].revents & POLLIN)) {
            uint64_t exp;
            (void)read(g_healthfd, &exp, sizeof(exp));
            run_health_checks();
        }
        if (g_fanfd >= 0 && (pfds[4].revents & POLLIN)) handle_fanotify_event();
        if (g_notifyfd >= 0 && (pfds[5].revents & POLLIN)) handle_notify_event();
        if (g_eventfd >= 0 && (pfds[6].revents & POLLIN)) handle_event_subscriber();
        event_flush_all();  /* v2.1: drain subscriber buffers each loop iter */
    }

    LOGI("shutdown signal received");
    shutdown_all();
    reap_children();
    unlink(g_ctl_path);
    unlink(g_event_path);  /* v2.1: clean up event socket */
    sync();
    closelog();  /* v2.1: flush syslog connection before reboot */
    /* deferred: PID 1 returning from main = kernel panic ("Attempted to kill
     * init!"). Must call reboot(2) to actually halt. RB_POWER_OFF on real
     * hardware powers the machine off; on VMs without ACPI shutdown it falls
     * back to halt. RB_AUTOBOOT reboots. */
    if (g_reboot_target) reboot(RB_AUTOBOOT);
    else                 reboot(RB_POWER_OFF);
    /* only reached if reboot() failed (e.g. CAP_SYS_BOOT dropped) */
    LOGE("reboot syscall failed: %s — halting", strerror(errno));
    reboot(RB_HALT_SYSTEM);
    return 0;
}
