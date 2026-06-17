/* hz-event-logger — example plugin for Hoshizora's event socket.
 * Connects to /run/hoshizora/events, reads event lines, optionally runs a
 * command on lines matching a prefix.
 *
 * Build: cc -O2 -Wall -static -o hz-event-logger hz-event-logger.c
 * Run:   hz-event-logger [--run CMD] [--match PREFIX] [--log FILE]
 *
 * Examples:
 *   # Tail all events to stdout (debugging)
 *   hz-event-logger
 *
 *   # Log all events to /var/log/hoshizora-events.log
 *   hz-event-logger --log /var/log/hoshizora-events.log
 *
 *   # Run a script when any service FAILs
 *   hz-event-logger --match 'FAILED ' --run '/usr/local/bin/hz-on-fail'
 *
 *   # Send a desktop notification on SHUTDOWN
 *   hz-event-logger --match SHUTDOWN --run 'wall "system going down"'
 *
 * The --run command receives the full event line on stdin. Exit code is
 * ignored. deferred: no reconnect-on-disconnect, no buffering, no rate
 * limit. Add when a real plugin needs them.
 *
 * Why C and not shell: shell `nc -U` does this in one line, but nc -U is
 * not portable (OpenBSD vs traditional), and reading a Unix socket from
 * pure shell needs socat. C + libc is the lowest-common-denominator dep.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--run CMD] [--match PREFIX] [--log FILE] [--socket PATH]\n"
        "\n"
        "  --socket PATH  event socket (default: $HZ_EVENT_PATH or /run/hoshizora/events)\n"
        "  --log FILE     append all events to FILE (default: stdout)\n"
        "  --match PREFIX only run CMD for lines starting with PREFIX (default: run for all)\n"
        "  --run CMD      shell command to run for matching events; event line on stdin\n",
        prog);
}

int main(int argc, char **argv) {
    const char *sock_path = getenv("HZ_EVENT_PATH");
    if (!sock_path || !*sock_path) sock_path = "/run/hoshizora/events";
    const char *log_path = NULL;      /* default: stdout */
    const char *run_cmd = NULL;
    const char *match_prefix = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--match") == 0 && i + 1 < argc) {
            match_prefix = argv[++i];
        } else if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
            run_cmd = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown arg: %s\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    /* connect to event socket */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", sock_path);
        return 1;
    }
    strcpy(addr.sun_path, sock_path);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror(sock_path);
        return 1;
    }

    /* deferred: SIGCHLD=SIG_IGN so --run children are auto-reaped by the
     * kernel. Without this, WNOHANG leaves zombies if the child outlives
     * the next event arrival. */
    signal(SIGCHLD, SIG_IGN);

    /* open log file if specified */
    int logfd = -1;
    if (log_path) {
        logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd < 0) { perror(log_path); return 1; }
    }

    /* read event lines, log + maybe run cmd */
    char buf[512];
    char line[512];
    size_t line_len = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read");
            break;
        }
        if (n == 0) break;  /* socket closed — hoshizora shutting down */
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || line_len >= sizeof(line) - 1) {
                line[line_len] = 0;
                /* log */
                if (logfd >= 0) {
                    (void)write(logfd, line, line_len);
                    (void)write(logfd, "\n", 1);
                } else {
                    printf("%s\n", line);
                    fflush(stdout);
                }
                /* maybe run cmd */
                if (run_cmd && (!match_prefix || strncmp(line, match_prefix, strlen(match_prefix)) == 0)) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        /* child — pipe event line to cmd's stdin */
                        int p[2];
                        if (pipe(p) < 0) { _exit(127); }
                        dup2(p[0], 0);
                        close(p[0]);
                        write(p[1], line, line_len);
                        write(p[1], "\n", 1);
                        close(p[1]);
                        execl("/bin/sh", "sh", "-c", run_cmd, NULL);
                        _exit(127);
                    } else if (pid > 0) {
                        /* SIGCHLD=SIG_IGN → kernel auto-reaps; no waitpid needed. */
                    }
                }
                line_len = 0;
            } else {
                line[line_len++] = c;
            }
        }
    }
    close(fd);
    if (logfd >= 0) close(logfd);
    return 0;
}
