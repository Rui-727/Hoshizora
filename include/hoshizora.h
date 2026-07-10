/*
 * HOSHIZORA, init system (PID 1) for Linux.
 * deferred: minimum viable init. Parses system.hs, forks/execs services,
 * restarts crashed ones, handles SIGTERM/SIGINT for shutdown.
 */
#ifndef HOSHIZORA_H
#define HOSHIZORA_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define HZ_MAX_SERVICES   64
#define HZ_MAX_WATCHES    32
#define HZ_MAX_TARGETS    16
#define HZ_MAX_ARGS       32
#define HZ_MAX_DEPS       8
#define HZ_MAX_ENV        32
#define HZ_MAX_BINDS      8
#define HZ_MAX_LISTENS    4
#define HZ_MAX_NAME       64
#define HZ_MAX_PATH       256
#define HZ_MAX_STR        256
#define HZ_MAX_VARS       32

typedef enum {
    HZ_S_STOPPED = 0,
    HZ_S_RUNNING,
    HZ_S_FAILED
} hz_state_t;

/* deferred: start-condition supports 3 builtins, joined by `and` (max 2).
 * No OR, no arithmetic, no if/then/else. Covers the actual configs in
 * system.hs (file-exists + link-up). Add operators when a real config needs them. */
typedef enum {
    HZ_SC_NONE = 0,
    HZ_SC_FILE_EXISTS,
    HZ_SC_LINK_UP,
    HZ_SC_FS_MOUNTED
} hz_sc_kind_t;

typedef struct {
    hz_sc_kind_t kind1;        /* HZ_SC_NONE = no condition */
    char         arg1[HZ_MAX_PATH];
    hz_sc_kind_t kind2;        /* HZ_SC_NONE = no second condition */
    char         arg2[HZ_MAX_PATH];
} hz_sc_t;

/* deferred: healthy: tcp-probe("host:port", Ns) only. One builtin. */
typedef struct {
    char hostport[HZ_MAX_STR]; /* "127.0.0.1:80", empty = no probe */
    int  timeout_s;
    int  fail_count;           /* runtime: consecutive failures */
} hz_health_t;

/* deferred: on-fail: restart(name) | shutdown. Fires when service transitions
 * to FAILED (exec-fail / backoff exhausted / dep failure / fork failure).
 * No chains, no nesting. Operator wrote the config, cycles are their bug. */
typedef enum {
    HZ_ON_FAIL_NONE = 0,
    HZ_ON_FAIL_RESTART,   /* start target service (stop first if running) */
    HZ_ON_FAIL_SHUTDOWN   /* set g_shutdown = 1 */
} hz_on_fail_kind_t;

typedef struct {
    hz_on_fail_kind_t kind;
    char              target[HZ_MAX_NAME];  /* for RESTART */
} hz_on_fail_t;

typedef enum {
    HZ_W_RELOAD = 0,
    HZ_W_RESTART
} hz_watch_action_t;

/* v2.0: container integration. deferred: no image format, no layer storage,
 * no OCI runtime compat. That's containerd's job. Hoshizora just supervises
 * the resulting process tree. */
typedef struct {
    int  new_ns;              /* 1 = unshare(CLONE_NEWNS|NEWNET|NEWPID|NEWIPC|NEWUTS) */
    char rootfs[HZ_MAX_PATH]; /* empty = no pivot_root */
    char binds[HZ_MAX_BINDS][HZ_MAX_PATH]; /* "src:dst" entries, mounted before exec */
    int  n_binds;
    int  readonly;            /* v2.2: 1 = remount rootfs MS_RDONLY after pivot */
} hz_container_t;

typedef struct {
    char     name[HZ_MAX_NAME];
    char     exec[HZ_MAX_PATH];
    char     argv[HZ_MAX_ARGS][HZ_MAX_STR];
    int      argc;
    char     deps[HZ_MAX_DEPS][HZ_MAX_NAME];
    int      n_deps;
    int      respawn;            /* deferred: presence of respawn: directive */
    int      max_restarts;       /* deferred: from backoff(max=N), default 5 */
    char     env_keys[HZ_MAX_ENV][HZ_MAX_NAME];
    char     env_vals[HZ_MAX_ENV][HZ_MAX_STR];
    int      n_env;
    unsigned long long memory_limit; /* bytes; 0 = no limit (cgroup v2 memory.max) */
    int      cpu_weight;             /* 1-10000; 0 = default (cgroup v2 cpu.weight) */
    int      oom_kill_group;         /* deferred: write memory.oom.group=1, kill whole cgroup on OOM, not one proc */
    hz_sc_t  start_cond;             /* deferred: file-exists / link-up / fs-mounted */
    hz_health_t health;              /* deferred: tcp-probe only */
    char     log_path[HZ_MAX_PATH];  /* deferred: per-service stdout/stderr redirect; empty = inherit */
    hz_on_fail_t on_fail;            /* deferred: action when service goes FAILED */
    int      cron_interval;     /* deferred: every: "Ns", 0 = not a cron job. Cron jobs are
                                 * one-shot: fork+exec, on clean exit re-arm for now+interval,
                                 * on crash use respawn logic / on-fail. Cron-syntax
                                 * `0 3 * * *` deferred, date math, add when a real config needs it. */
    int      timeout_start;     /* v2.0: timeout-start: Ns, if not RUNNING within N seconds, FAILED. 0 = no timeout. */
    int      retry_after;       /* v2.0: retry-after: Ns, for services with start-condition that fail
                                 * to execve (exit 127). Re-arms cron_next. Default 5s. */
    int      expect_notify;     /* v2.0: 1 = service speaks sd_notify (READY=1) over the global notify socket.
                                 * Pairs with timeout_start, timer disarms on READY. */
    hz_container_t container;   /* v2.0: namespace + rootfs + binds */
    char     listens[HZ_MAX_LISTENS][HZ_MAX_STR]; /* v2.0: socket activation, "0.0.0.0:80" or "/path/to/unix.sock" */
    /* v2.2: cgroup v2 extras */
    int      io_weight;         /* 1-10000; 0 = default (cgroup v2 io.weight) */
    unsigned long long memory_high; /* bytes; 0 = no soft limit (cgroup v2 memory.high) */
    int      cpu_max_quota;     /* microseconds per period; 0 = no quota (cgroup v2 cpu.max "quota period") */
    int      cpu_max_period;    /* period in microseconds; default 100000 (100ms) */
    /* v2.2: security */
    int      no_new_privs;      /* 1 = prctl(PR_SET_NO_NEW_PRIVS) in child before exec */
    uid_t    run_as_uid;        /* 0 = inherit; else setuid before exec */
    gid_t    run_as_gid;        /* 0 = inherit; else setgid before exec */
    /* v2.2: lifecycle hooks */
    char     pre_start[HZ_MAX_PATH];  /* shell command, run before start_service forks */
    char     post_stop[HZ_MAX_PATH];  /* shell command, run after stop_service reaps */
    /* v2.2: watchdog, service sends WATCHDOG=1 every Ns, else FAILED */
    int      watchdog_timeout;  /* 0 = disabled; else max seconds between WATCHDOG=1 */

    /* runtime */
    hz_state_t state;
    pid_t      pid;
    int        restart_count;
    int        manual_stop;      /* set by `stop` cmd / shutdown, blocks respawn */
    time_t     respawn_at;       /* 0 = no pending respawn; else epoch seconds */
    time_t     cron_next;        /* 0 = not scheduled; else next fire time (mono_now units) */
    time_t     start_deadline;   /* 0 = no timeout; else mono_now() seconds by which state must be RUNNING */
    int        listen_fds[HZ_MAX_LISTENS]; /* bound listening fds, -1 = unused */
    int        n_listen_fds;
    int        notify_ready;     /* runtime: 1 = service sent READY=1 */
    time_t     watchdog_last;    /* runtime: mono_now() of last WATCHDOG=1 */
    /* v2.4: restart rate limiter. If a service exits within 1 second of
     * starting, that's a fast crash. After 5 fast crashes within a 30s
     * window, give up and mark_failed. Independent of max_restarts, which
     * counts ALL restarts (including slow ones). */
    time_t     last_start_mono;       /* mono_now() of last successful fork */
    time_t     fast_crash_window;     /* 0 = no window active; else window start */
    int        fast_crash_count;      /* crashes within the current window */
} hz_service_t;

typedef struct {
    char              path[HZ_MAX_PATH];
    char              service[HZ_MAX_NAME];
    hz_watch_action_t action;
    int               recursive;  /* v2.2: 1 = FAN_MARK_FILESYSTEM (whole fs) */
    /* deferred: `recursive` keyword accepted in config for compatibility but
     * not honored. fanotify marks top dir only. Add FAN_MARK_FILESYSTEM or
     * recursive mount-mark logic if a real config needs it. */
} hz_watch_t;

/* v2.0: target, named collection of services. hzctl start <target> walks
 * the list. deferred: isolate command (stop everything not in dep closure),
 * target-on-target deps. Add when 10+ services in a real config. */
typedef struct {
    char name[HZ_MAX_NAME];
    char services[HZ_MAX_DEPS][HZ_MAX_NAME];
    int  n_services;
} hz_target_t;

typedef struct {
    hz_service_t services[HZ_MAX_SERVICES];
    int          n_services;
    hz_watch_t   watches[HZ_MAX_WATCHES];
    int          n_watches;
    hz_target_t  targets[HZ_MAX_TARGETS];
    int          n_targets;
    int          shutting_down;
} hz_system_t;

#endif /* HOSHIZORA_H */
