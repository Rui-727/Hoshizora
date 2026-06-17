/* hzctl — tiny control client for the hoshizora init system.
 * deferred: one file, one job. Connects to a Unix socket, sends one line
 * from argv, prints response. No flags except --force for shutdown gating.
 *
 * Usage: hzctl <command> [args...]
 *   hzctl list
 *   hzctl status
 *   hzctl status nginx
 *   hzctl start nginx
 *   hzctl stop nginx
 *   hzctl restart nginx
 *   hzctl reload
 *   hzctl shutdown [--force]    # refuses if /run/hoshizora/sessions/* has entries
 *
 * Default socket: /run/hoshizora/control (override with HZ_CTL_PATH env).
 * Default session dir: /run/hoshizora/sessions (override with HZ_SESSION_DIR).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>

/* v2.1: count entries in the session dir. Returns -1 if dir missing (no
 * sessions, fine to proceed), or count of regular files otherwise. */
static int count_sessions(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n++;
    }
    closedir(d);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> [args...]\n", argv[0]);
        return 2;
    }

    /* v2.1: shutdown gate — refuse if sessions exist, unless --force.
     * Skip the check for non-shutdown commands (cheap: just one strcmp). */
    if (strcmp(argv[1], "shutdown") == 0 || strcmp(argv[1], "poweroff") == 0
        || strcmp(argv[1], "reboot") == 0) {
        int force = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0) { force = 1; break; }
        }
        if (!force) {
            const char *sdir = getenv("HZ_SESSION_DIR");
            if (!sdir || !*sdir) sdir = "/run/hoshizora/sessions";
            int n = count_sessions(sdir);
            if (n > 0) {
                fprintf(stderr,
                    "hzctl: refusing shutdown — %d open session(s) in %s\n"
                    "  run 'hzctl shutdown --force' to override\n", n, sdir);
                return 3;
            }
        }
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

    /* build command line: arg1 arg2 ... — strip any --force so the server
     * doesn't have to know about it. */
    char buf[1024];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) continue;
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
