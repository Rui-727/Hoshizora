/* hzlog, deferred syslog collector.
 * deferred: one job. Bind /dev/log, recv datagrams, prepend receive-time,
 * append to /var/log/messages. No rotation (logrotate's job), no per-facility
 * split (awk's job), no PRI parsing (glibc syslog(3) already formats the line).
 *
 * Build: cc -O2 -Wall -static -o hzlog hzlog.c
 * Run:   ./hzlog   (as root, after PID 1 starts)
 *        Override log path with $HZ_LOG_FILE.
 *        Override socket path with $HZ_LOG_SOCK (default /dev/log).
 *
 * Why a separate binary from hoshizora: PID 1 must not parse untrusted input.
 * Any process can send a syslog datagram; a parser bug here can't crash PID 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define HZLOG_BUF  8192   /* max RFC 3164 line is 1024; we accept larger just in case */

int main(void) {
    const char *sock_path = getenv("HZ_LOG_SOCK");
    if (!sock_path || !*sock_path) sock_path = "/dev/log";
    const char *log_path = getenv("HZ_LOG_FILE");
    if (!log_path || !*log_path) log_path = "/var/log/messages";

    /* bind /dev/log as datagram socket. unlink first (stale socket from a
     * previous crash). mkdir -p parent only if non-default path used in
     * tests. */
    unlink(sock_path);
    int sfd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "hzlog: socket path too long: %s\n", sock_path);
        return 1;
    }
    strcpy(addr.sun_path, sock_path);
    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    /* deferred: 0666 so any UID can log. syslog is a trust boundary by
     * convention; anyone on the box can already write to it via logger(1).
     * Tighten with chown+chmod when running multi-tenant. */
    chmod(sock_path, 0666);

    /* open log file O_APPEND so concurrent writers (e.g. hzlog restart) don't
     * clobber each other. Line-buffered via dprintf, no FILE* buffering
     * because we want crash survival. */
    int lfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (lfd < 0) {
        perror(log_path);
        return 1;
    }

    fprintf(stderr, "hzlog: listening on %s, writing to %s\n", sock_path, log_path);

    char buf[HZLOG_BUF];
    char tstamp[32];
    for (;;) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            continue;  /* deferred: don't die on a bad recv; one bad client shouldn't kill the log daemon */
        }
        if (n == 0) continue;
        buf[n] = 0;
        /* strip trailing newline so we don't get double-spaced output when
         * glibc sends `<PRI>... MSG\n` */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        /* prepend receive-time (RFC 3164 has its own timestamp from the
         * sender, but we add ours for ordering; datagrams can arrive out of
         * order under load). */
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(tstamp, sizeof(tstamp), "%b %e %H:%M:%S", &tm);
        /* one dprintf; appended line: "<tstamp> <raw datagram>\n".
         * deferred: dprintf replaces snprintf+write pair + the 8KB stack buffer. */
        int ln = dprintf(lfd, "%s %s\n", tstamp, buf);
        if (ln < 0 && errno != EINTR) {
            perror("write");
            /* deferred: keep going; log daemon stopping is worse than losing a line. */
        }
    }
    /* not reached; recv loop above never exits */
}
