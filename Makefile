# HOSHIZORA - init system (PID 1) for Linux/x86_64.
# deferred: one binary, no bootloader, no host compiler, no .hsb step.
# Builds ./hoshizora. Install as /sbin/init or run as PID 1.
#
# musl is preferred (85% smaller static binaries). Falls back to gcc+glibc
# if musl-gcc is not installed.
#   make            → auto-detect musl-gcc, fall back to gcc
#   make CC=musl-gcc → force musl
#   make CC=gcc      → force glibc

CC      ?= $(shell if musl-gcc --version >/dev/null 2>&1; then echo musl-gcc; else echo gcc; fi)
CFLAGS  = -Wall -Wextra -O2 -Iinclude -D_GNU_SOURCE
LDFLAGS = -static

.PHONY: all clean test install uninstall

all: hoshizora hzctl hzlog hz-event-logger

hoshizora: src/init.c include/hoshizora.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/init.c

hzctl: src/hzctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/hzctl.c

hzlog: src/hzlog.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/hzlog.c

hz-event-logger: src/hz-event-logger.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/hz-event-logger.c

# deferred: ONE testsuite entry point. Runs each self-check in sequence,
# tallies PASS/FAIL. Each check has its own .hs config covering a slice:
#   tests/core.sh     + tests/core.hs     — parse, fork+exec, deps, exec-fail, CLI verbs
#   tests/features.sh + tests/features.hs — parser + runtime for cgroup/watch/start-cond/
#                                           healthy/backoff-max/oom-kill/log/network_ready
#   tests/onfail.sh   + tests/onfail.hs   — on-fail: directive (parser + runtime firing)
#   tests/cron.sh     + tests/cron.hs     — every: directive (cron-style interval scheduling)
#   tests/hzlog.sh    (no .hs)            — hzlog syslog collector: send line, read back
#   tests/v2.sh       + tests/v2.hs       — v2.0: vars, timeout-start, listen, target, caps, notify
#   tests/v21.sh      + tests/v21.hs      — v2.1: plugin event socket, shutdown gate
#   tests/plugin.sh   + tests/v21.hs      — hz-event-logger: subscribe + --match + --run
#   tests/edge_cases.sh (no .hs, generates its own) — signal safety, write_all,
#                                           cgroup leak prevention, restart rate limiter
test: hoshizora hzctl hzlog hz-event-logger tests/testsuite.sh \
      tests/core.sh tests/features.sh tests/onfail.sh tests/cron.sh tests/hzlog.sh tests/v2.sh tests/v21.sh tests/plugin.sh tests/edge_cases.sh \
      tests/core.hs tests/features.hs tests/onfail.hs tests/cron.hs tests/v2.hs tests/v21.hs
	@bash tests/testsuite.sh

clean:
	rm -f hoshizora hzctl hzlog hz-event-logger

# v2.4: install target supports DESTDIR + PREFIX so distro packaging (PKGBUILD,
# rpm spec, Makefile.packages) can stage into a fakeroot. Defaults match the
# existing layout (PREFIX=/usr, sbindir=/sbin) so `sudo make install` still
# works without args. Installs binaries, header, scripts, and completions.
PREFIX      ?= /usr
SBINDIR     ?= /sbin
BINDIR      ?= $(PREFIX)/bin
SYSCONFDIR  ?= /etc
INCLUDEDIR  ?= $(PREFIX)/include
LIBDIR      ?= $(PREFIX)/lib/hoshizora
COMPLETIONSDIR ?= $(PREFIX)/share/bash-completion/completions
ZSHDIR      ?= $(PREFIX)/share/zsh/site-functions
DESTDIR     ?=

install: hoshizora hzctl hzlog hz-event-logger
	install -dD $(DESTDIR)$(SBINDIR) \
	           $(DESTDIR)$(BINDIR) \
	           $(DESTDIR)$(INCLUDEDIR) \
	           $(DESTDIR)$(SYSCONFDIR)/hoshizora \
	           $(DESTDIR)$(LIBDIR) \
	           $(DESTDIR)$(COMPLETIONSDIR) \
	           $(DESTDIR)$(ZSHDIR)
	# Binaries: init goes to /sbin (boot-time path), clients to /usr/bin.
	install -m 0755 hoshizora        $(DESTDIR)$(SBINDIR)/hoshizora
	install -m 0755 hzctl            $(DESTDIR)$(BINDIR)/hzctl
	install -m 0755 hzlog            $(DESTDIR)$(BINDIR)/hzlog
	install -m 0755 hz-event-logger  $(DESTDIR)$(BINDIR)/hz-event-logger
	# Header: needed by C plugins that link against the event-socket helpers.
	install -m 0644 include/hoshizora.h $(DESTDIR)$(INCLUDEDIR)/hoshizora.h
	# pam_exec helper: install under /etc/hoshizora/ (config) + /usr/lib/hoshizora
	# (canonical libexec path) so distros can pick whichever FHS flavor they use.
	install -m 0755 scripts/hz-session  $(DESTDIR)$(LIBDIR)/hz-session
	# Optional systemctl-compat wrapper. Disabled by default to avoid shadowing
	# a real systemctl on hybrid boxes; uncomment to install.
	install -m 0755 scripts/hzctl-systemctl $(DESTDIR)$(LIBDIR)/hzctl-systemctl
	# Bash + zsh completions.
	install -m 0644 scripts/hzctl-completion.bash $(DESTDIR)$(COMPLETIONSDIR)/hzctl
	install -m 0644 scripts/hzctl-completion.zsh  $(DESTDIR)$(ZSHDIR)/_hzctl
	@echo "Installed: $(SBINDIR)/hoshizora, $(BINDIR)/{hzctl,hzlog,hz-event-logger},"
	@echo "           $(INCLUDEDIR)/hoshizora.h, $(LIBDIR)/{hz-session,hzctl-systemctl},"
	@echo "           $(COMPLETIONSDIR)/hzctl, $(ZSHDIR)/_hzctl"
	@echo "Set as init= in kernel cmdline, or: ln -sf $(SBINDIR)/hoshizora /sbin/init"

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/hoshizora
	rm -f $(DESTDIR)$(BINDIR)/hzctl $(DESTDIR)$(BINDIR)/hzlog $(DESTDIR)$(BINDIR)/hz-event-logger
	rm -f $(DESTDIR)$(INCLUDEDIR)/hoshizora.h
	rm -f $(DESTDIR)$(LIBDIR)/hz-session $(DESTDIR)$(LIBDIR)/hzctl-systemctl
	rm -f $(DESTDIR)$(COMPLETIONSDIR)/hzctl
	rm -f $(DESTDIR)$(ZSHDIR)/_hzctl
