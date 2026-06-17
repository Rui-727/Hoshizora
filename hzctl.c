/* hzctl — tiny control client for the hoshizora init system.
 * ponytail: one file, one job. Connects to a Unix socket, sends one line
 * from argv, prints response. No flags, no framing, no library deps.
 *
 * Usage: hzctl <command> [args...]
 *   hzctl list
 *   hzctl status
 *   hzctl status nginx
 *   hzctl start nginx
 *   hzctl stop nginx
 *   hzctl restart nginx
 *   hzctl reload
 *   hzctl shutdown
 *
 * Default socket: /run/hoshizora/control (override with HZ_CTL_PATH env).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> [args...]\n", argv[0]);
        return 2;
    }
    const char *path = getenv("HZ_CTL_PATH");
    if (!path || !*path) path = "/run/hoshizora/control";

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror(path);
        return 1;
    }

    /* build command line: arg1 arg2 ... */
    char buf[1024];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        int w = snprintf(buf + n, sizeof(buf) - n - 1, i > 1 ? " %s" : "%s", argv[i]);
        if (w < 0 || (size_t)w >= sizeof(buf) - n - 1) break;
        n += w;
    }
    buf[n++] = '\n';
    if (write(fd, buf, n) != n) { perror("write"); close(fd); return 1; }
    /* shutdown write side so server sees EOF */
    shutdown(fd, SHUT_WR);

    /* print response */
    char rbuf[4096];
    ssize_t r;
    while ((r = read(fd, rbuf, sizeof(rbuf))) > 0) {
        (void)write(1, rbuf, r);
    }
    close(fd);
    return 0;
}
