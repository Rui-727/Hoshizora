# HOSHIZORA - init system (PID 1) for Linux/x86_64.
# deferred: one binary, no bootloader, no host compiler, no .hsb step.
# Builds ./hoshizora. Install as /sbin/init or run as PID 1.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude -D_GNU_SOURCE
LDFLAGS = -static

.PHONY: all clean test install

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
test: hoshizora hzctl hzlog hz-event-logger tests/testsuite.sh \
      tests/core.sh tests/features.sh tests/onfail.sh tests/cron.sh tests/hzlog.sh tests/v2.sh tests/v21.sh tests/plugin.sh \
      tests/core.hs tests/features.hs tests/onfail.hs tests/cron.hs tests/v2.hs tests/v21.hs
	@bash tests/testsuite.sh

clean:
	rm -f hoshizora hzctl hzlog hz-event-logger

install: hoshizora hzctl hzlog hz-event-logger
	cp hoshizora /sbin/hoshizora
	cp hzctl /usr/bin/hzctl
	cp hzlog /usr/bin/hzlog
	cp hz-event-logger /usr/bin/hz-event-logger
	# Shell scripts (hz-session, hzctl-systemctl, completions) are not auto-installed.
	# Copy manually: scripts/hz-session → /etc/hoshizora/hz-session (pam_exec)
	#                scripts/hzctl-systemctl → /usr/local/bin/systemctl (optional wrapper)
	#                scripts/hzctl-completion.bash → /etc/bash_completion.d/hzctl
	#                scripts/hzctl-completion.zsh → /usr/share/zsh/site-functions/_hzctl
	@echo "Installed to /sbin/hoshizora - set as init= in kernel cmdline or symlink /sbin/init"
