# Hoshizora — Getting Started Guide

This guide walks you through testing Hoshizora safely, from "I just cloned
the repo" to "I booted it as PID 1 in QEMU and got a login prompt".

**If this is your only computer:** stop at level 2 (user namespace), or
jump to level 4 (LFS in QEMU) if you want the real PID-1 experience.
Levels 1, 2, and 4 are zero-risk — they never touch your boot, your real
init, or your kernel cmdline. Level 5 (real hardware) has real brick risk;
don't do it on a one-laptop setup unless you've proven your recovery path
on someone else's machine.

Testing levels, safest first:

1. **Sandbox** — `make test`, runs the self-check suite. No root, no risk.
2. **User namespace** — `unshare -r -p -f`, fake-root PID 1 in a namespace.
   No root needed, no boot changes, no real cgroups/devices.
3. **QEMU + initramfs** — boot a real (host) kernel with Hoshizora as init.
   Full PID 1 experience, isolated VM, no real hardware risk.
4. **Linux From Scratch (LFS) in QEMU** — build a complete Linux system
   from source in a chroot, substitute hoshizora for systemd at chapter
   8.74, boot it in QEMU. The best real-PID-1 test bed. Zero risk to host.
5. **Real hardware** — install on a test box or spare partition. Real risk,
   real reward. **Don't do this on your only box.**

---

## 0. Prerequisites

```bash
# Build tools (Debian/Ubuntu)
sudo apt install build-essential

# For QEMU testing (optional but recommended)
sudo apt install qemu-system-x86 busybox-static linux-image-amd64 cpio gzip

# For real-hardware testing (optional)
sudo apt install util-linux                  # for agetty, mount, hostname
sudo apt install iproute2                    # for ip (loopback)
```

Build + self-check:

```bash
git clone https://github.com/Rui-727/Hoshizora.git
cd Hoshizora
make
make test
```

Expected: `total PASS: 65, total FAIL: 0, all checks passed.`

If any test fails, stop and report before proceeding. The self-checks
exercise every code path that doesn't need root.

---

## 1. Sandbox testing (no root, no risk)

`make test` runs 8 self-checks:

| Script              | What it exercises                                   |
|---------------------|-----------------------------------------------------|
| `tests/core.sh`     | Parse, fork+exec, deps, exec-fail, all CLI verbs (SOV), enable/disable/show/logs/daemon-reload |
| `tests/features.sh` | memory-limit, cpu-weight, oom-kill, log, start-condition, watch, backoff(max=N), network_ready |
| `tests/onfail.sh`   | on-fail: directive, respawn timer, backoff guard, clean exit via on-fail: shutdown |
| `tests/cron.sh`     | every: cron-style interval scheduling + re-arm      |
| `tests/hzlog.sh`    | hzlog syslog collector: send datagram, read back    |
| `tests/v2.sh`       | v2.0: variable substitution, timeout-start, listen (socket activation), target, caps, notify |
| `tests/v21.sh`      | v2.1: event socket, subscriber HELLO+START, shutdown gate |
| `tests/plugin.sh`   | hz-event-logger: subscribe, log events, receive SHUTDOWN |

Run an individual check for debugging:

```bash
bash tests/core.sh
bash tests/v21.sh
```

Each script cleans up its temp files on exit. Logs go to `/tmp/hz_*.log`
during the run and are removed at the end (unless the test fails — then
the log is left for inspection).

---

## 2. User namespace testing (no root, fake PID 1)

This is the sweet spot for testing: you become PID 1 of a new PID
namespace, with fake root via user namespaces. No `sudo` needed.

```bash
# 1. Create a safe socket directory (don't touch /run)
mkdir -p ~/.local/run

# 2. Tell Hoshizora to use your home directory for sockets
export HZ_CTL_PATH="$HOME/.local/run/control"
export HZ_EVENT_PATH="$HOME/.local/run/events"
export HZ_NOTIFY_PATH="$HOME/.local/run/notify"

# 3. Run in a user namespace, faking root
#    -r = map current UID to root inside the ns
#    -p = new PID namespace (we become PID 1)
#    -f = fork so the new PID 1 is hoshizora, not unshare itself
#    NOTE: do NOT use --mount-proc — it needs CAP_SYS_ADMIN in the
#    parent namespace, which non-root doesn't have.
unshare -r -p -f ./hoshizora system.hs &
HZ_PID=$!

# 4. Wait for the control socket to appear
for i in $(seq 1 10); do
    [ -S "$HZ_CTL_PATH" ] && break
    sleep 0.2
done

# 5. Talk to it (SOV — Subject Object Verb)
./hzctl list
./hzctl nginx status
./hzctl logs 20

# 6. Subscribe to events (in another terminal)
export HZ_EVENT_PATH="$HOME/.local/run/events"
./hz-event-logger                       # tail events to stdout
# or: ./hz-event-logger --log /tmp/hz-events.txt

# 7. Clean shutdown
./hzctl shutdown
wait $HZ_PID

# Cleanup
rm -f ~/.local/run/{control,events,notify}
```

### What works in user-namespace mode

- ✅ Control socket + `hzctl` (SOV)
- ✅ Service lifecycle (fork+exec, respawn, on-fail)
- ✅ `capset(2)` — works inside the user namespace (you have CAP_SYS_ADMIN etc. *inside* the ns)
- ✅ Event socket + plugins
- ✅ sd-notify socket + READY=1 parsing
- ✅ Config parsing (all fields)
- ✅ Variable substitution (`$NAME = "value";`)
- ✅ Cron scheduling (`every:`)
- ✅ Targets (`hzctl <target> start`)

### What degrades gracefully

- ⚠️ **fanotify** — `fanotify_init` returns EPERM without real CAP_SYS_ADMIN. Hoshizora logs a warning and watches become inert. Services still run, just no auto-reload on file changes.
- ⚠️ **cgroup v2** — no cgroupfs access from the user namespace. Hoshizora logs a one-time warning and skips `memory.max`/`cpu.weight`/`io.weight`/`memory.high`/`cpu.max` writes.
- ⚠️ **reboot(2)** — works inside the ns but doesn't actually reboot anything (the ns just exits). Hoshizora logs "reboot syscall failed" and falls through to halt. Tests assert this log line as proof the syscall was attempted.
- ⚠️ **pivot_root / unshare inside a service** — `namespace: private` on a service tries `unshare(CLONE_NEWNS|NEWNET|NEWPID|NEWIPC|NEWUTS)` from inside the already-namespaced hoshizora. May fail with EPERM; hoshizora logs a warning and the service runs without isolation.

### What doesn't work

- ❌ Real PID 1 semantics (kernel hands init signal handling, can't be killed)
- ❌ Real cgroup enforcement
- ❌ Real fanotify watches
- ❌ Actual poweroff/reboot of the host

---

## 3. QEMU + initramfs (full PID 1, isolated VM)

This is how kernel developers test init systems. Boots a real kernel with
Hoshizora as `/init` in a minimal initramfs.

```bash
# Prereqs (Debian/Ubuntu)
sudo apt install qemu-system-x86 busybox-static linux-image-amd64 cpio gzip

# Build + boot
make
./tests/qemu.sh
```

`tests/qemu.sh` does the following automatically:

1. Builds `./hoshizora` (static binary)
2. Creates a minimal initramfs at `/tmp/hz-initramfs.cpio.gz` containing:
   - `/init` → hoshizora binary
   - `/bin/busybox` + symlinks for `sh`, `mount`, `agetty`, `hostname`, `ip`, etc.
   - `/etc/hoshizora/system.hs` → copied from `system.bootable.hs`
   - `/etc/passwd`, `/etc/shadow` (root, no password), `/etc/fstab`, `/etc/hostname`
3. Boots QEMU with the host kernel + the initramfs:
   ```
   qemu-system-x86_64 \
       -kernel /boot/vmlinuz-* \
       -initrd /tmp/hz-initramfs.cpio.gz \
       -append "console=ttyS0 init=/init panic=-1" \
       -nographic -m 512M -no-reboot
   ```

### What you'll see

```
[kernel boot messages scroll by]
[hoshizora I] hoshizora starting, config=/etc/hoshizora/system.hs
[hoshizora I] loaded service mount-root (exec=/bin/mount, 0 deps, ...)
[hoshizora I] loaded service getty-tty1 (exec=/sbin/agetty, ...)
[hoshizora I] started mount-root (pid=12)
[hoshizora I] started getty-tty1 (pid=15)

hoshizora-box login:
```

Type `root`, no password. You're in. Hoshizora is PID 1. Try:

```bash
hzctl list                     # see what's running
hzctl getty-tty1 status        # check the login service
hzctl getty-tty1 stop          # kill your own login (respawn brings it back)
dmesg | tail                   # kernel messages
ps aux                         # process tree (hoshizora is PID 1)
```

### To exit QEMU

Press `Ctrl-A` then `X`. The VM stops immediately. The initramfs at
`/tmp/hz-initramfs.cpio.gz` is left behind for inspection; delete it with
`rm /tmp/hz-initramfs.cpio.gz` when done.

### Debugging a boot failure

If hoshizora panics on boot (kernel says "Attempted to kill init!"), add
`init=/bin/sh` to the kernel command line in `tests/qemu.sh` to bypass
hoshizora and get a shell:

```bash
# In tests/qemu.sh, change the -append line:
-append "console=ttyS0 init=/bin/sh panic=-1"
```

Then from the shell, you can run `/init` manually to see the error:

```sh
/init
# or with strace (if you include it in the initramfs):
strace /init
```

---

## 4. Linux From Scratch (safe real-PID-1 test bed)

[LFS](https://www.linuxfromscratch.org/lfs/) is the ideal way to test
Hoshizora as a real PID 1 without touching your daily driver. You build
an entire Linux system from source in a chroot on your host, then boot it
in QEMU. The LFS system is fully isolated — your Arch install is never
at risk.

**Why LFS is the right call here:**
- You choose the init system (LFS chapter 8.78 builds systemd by default — skip it, install hoshizora instead)
- You build it in a chroot — your host kernel + systemd keep running your laptop
- You boot the result in QEMU — full kernel-handoff-to-init experience, no real hardware
- If it breaks: delete the LFS partition/directory, start over. Zero recovery needed.

### 4.1 Follow the LFS book up to chapter 8.77

Build LFS exactly per the book (https://www.linuxfromscratch.org/lfs/view/stable-systemd/).
Stop before chapter 8.78 (Systemd-259.1). You'll have:
- A complete base system in `/mnt/lfs` (or wherever you put it)
- GRUB installed in the LFS root (chapter 8.66)
- Coreutils, bash, util-linux, iproute2 (chapter 8.68), kbd (chapter 8.69),
  tar (chapter 8.73), etc. all built and installed

Then continue through chapter 9 (System Configuration) and chapter 10
(Making the LFS System Bootable), but skip the systemd-specific parts:

- **Chapter 9 — do everything EXCEPT** 9.10 (Systemd Usage and Configuration)
- **Chapter 10 — do all of it:**
  - 10.2 Creating the /etc/fstab File
  - 10.3 Linux-6.18.10 (build the kernel)
  - 10.4 Using GRUB to Set Up the Boot Process

After chapter 10 you'll have:
- A kernel at `/boot/vmlinuz` (inside the LFS root)
- A valid `/etc/fstab` (inside the LFS root)
- GRUB configured (we won't use it for QEMU, but it doesn't hurt)

### 4.2 Substitute hoshizora for systemd

Instead of chapter 8.78 (building Systemd-259.1), do this:

```bash
# Enter the LFS chroot (per LFS book chapter 7.4 — exact command from the book)
sudo chroot "$LFS" /usr/bin/env -i   \
    HOME=/root                  \
    TERM="$TERM"                \
    PS1='(lfs chroot) \u:\w\$ ' \
    PATH=/usr/bin:/usr/sbin     \
    /bin/bash --login

# Inside the chroot:
cd /sources
git clone https://github.com/Rui-727/Hoshizora.git
cd Hoshizora
make
make install   # hoshizora → /sbin/, hzctl/hzlog/hz-event-logger → /usr/bin/

# Make hoshizora the init
ln -sf /sbin/hoshizora /sbin/init

# Config
mkdir -p /etc/hoshizora /run/hoshizora/sessions
```

### 4.3 LFS-tuned config

```bash
cat > /etc/hoshizora/system.hs << 'EOF'
system "lfs" {
    # Remount root RW. LFS boots with root RO by default.
    service mount-root {
        exec: "/bin/mount" with args ["-o", "remount,rw", "/"];
        on-fail: shutdown;
    }

    # Mount everything in /etc/fstab
    service mount-all {
        exec: "/bin/mount" with args ["-a"];
        requires: [mount-root];
        on-fail: shutdown;
    }

    # LFS uses eudev (the systemd-udevd fork) — install it per BLFS if
    # you want device hotplug. If you skip eudev, kernel devtmpfs still
    # gives you basic /dev nodes.
    service udev {
        exec: "/sbin/udevd";
        requires: [mount-all];
        respawn: backoff(max = 5);
    }

    # Hostname
    service hostname {
        exec: "/bin/hostname" with args ["lfs"];
        requires: [mount-all];
    }

    # Loopback
    service net-lo {
        exec: "/sbin/ip" with args ["link", "set", "lo", "up"];
        requires: [mount-all];
        on-fail: shutdown;
    }

    # Getty on tty1 — login prompt
    service getty-tty1 {
        exec: "/sbin/agetty" with args ["tty1", "linux"];
        requires: [mount-all, hostname, udev];
        respawn: backoff(max = 5);
    }

    # Serial console for QEMU -nographic
    service getty-ttyS0 {
        exec: "/sbin/agetty" with args ["ttyS0", "115200", "linux"];
        requires: [mount-all, hostname, udev];
        respawn: backoff(max = 5);
    }

    # Persistent logging
    service hzlog {
        exec: "/usr/bin/hzlog";
        requires: [mount-all];
        respawn: backoff(max = 5);
    }
}
EOF
```

### 4.4 /etc/fstab for LFS

The LFS book has you create this. Make sure it includes:

```
/dev/sda1  /      ext4  defaults       0 1
proc       /proc  proc  defaults       0 0
sysfs      /sys   sysfs defaults       0 0
devtmpfs   /dev   devtmpfs defaults    0 0
tmpfs      /run   tmpfs defaults       0 0
```

### 4.5 /etc/passwd + /etc/shadow

LFS creates these. Verify root has a password (or no password for testing):

```bash
# Inside chroot:
passwd   # set root password, or just leave blank for testing
```

### 4.6 Boot in QEMU

LFS doesn't need a separate initramfs — the kernel mounts root directly
if your root filesystem is on a partition the kernel can find. For QEMU,
create a disk image from your LFS build:

```bash
# On your host (NOT in chroot):
# Create a 2GB sparse disk image
truncate -s 2G /tmp/lfs-disk.img

# Format + mount it
mkfs.ext4 /tmp/lfs-disk.img
sudo mkdir /mnt/lfs-disk
sudo mount -o loop /tmp/lfs-disk.img /mnt/lfs-disk

# Copy your LFS build into it
sudo cp -a /mnt/lfs/* /mnt/lfs-disk/
sudo umount /mnt/lfs-disk

# Boot in QEMU
qemu-system-x86_64 \
    -kernel /mnt/lfs/boot/vmlinuz \
    -append "root=/dev/sda console=ttyS0 init=/sbin/hoshizora panic=-1" \
    -drive file=/tmp/lfs-disk.img,format=raw,if=virtio \
    -nographic -m 1G -no-reboot
```

You should see kernel boot messages, then hoshizora log lines, then:

```
lfs login:
```

Login as root, and you're on a real LFS system with hoshizora as PID 1.

### 4.7 Why this is the right test bed

| Concern                       | LFS answer                                         |
|-------------------------------|----------------------------------------------------|
| Brick my laptop?              | No — LFS is in a chroot/disk image, host untouched |
| See real kernel→init handoff? | Yes — kernel boots, calls /sbin/init = hoshizora   |
| Real PID 1 semantics?         | Yes — hoshizora is actual PID 1 in the VM          |
| Real cgroups?                 | Yes — cgroup v2 works inside QEMU                  |
| Real fanotify?                | Yes — QEMU has CAP_SYS_ADMIN in the guest          |
| Recovery if broken?           | Delete /tmp/lfs-disk.img, rebuild. Zero downtime.  |
| Disk space?                   | 2GB image, fits in your 3.5GB free                 |
| RAM?                          | Give QEMU 1GB, leave 3GB for Arch + zram           |

### 4.8 Iterating

To change the config without rebuilding the disk image:

```bash
# Boot LFS in QEMU
# At the login prompt, login as root
vi /etc/hoshizora/system.hs
hzctl daemon-reload         # re-reads config, applies diff
hzctl list                  # see what changed
```

Or mount the image on the host and edit directly:

```bash
sudo mount -o loop /tmp/lfs-disk.img /mnt/lfs-disk
sudo vi /mnt/lfs-disk/etc/hoshizora/system.hs
sudo umount /mnt/lfs-disk
```

### 4.9 Cleaning up

When you're done:

```bash
rm /tmp/lfs-disk.img
sudo rm -rf /mnt/lfs   # if you want to delete the LFS build entirely
```

Your Arch install is untouched.

---

## 5. Real hardware (real risk)

For a test box or spare partition. **Do not do this on your daily driver
without a recovery plan.**

### Install

```bash
git clone https://github.com/Rui-727/Hoshizora.git
cd Hoshizora
make
sudo make install                          # hoshizora → /sbin/, hzctl + hzlog + hz-event-logger → /usr/bin/

# Config
sudo mkdir -p /etc/hoshizora
sudo cp system.bootable.hs /etc/hoshizora/system.hs
sudo vi /etc/hoshizora/system.hs           # adjust services for your box

# Session tracker (optional, for login ACLs)
sudo cp hz-session /etc/hoshizora/hz-session
sudo chmod 0755 /etc/hoshizora/hz-session
sudo mkdir -p /run/hoshizora/sessions
# Add to /etc/pam.d/login:
#   session required pam_exec.so /etc/hoshizora/hz-session
```

### Set as init

Two options:

```bash
# Option A: kernel cmdline (recommended — easy to override)
# Add to /etc/default/grub:
#   GRUB_CMDLINE_LINUX="init=/sbin/hoshizora"
sudo update-grub

# Option B: symlink (harder to override)
sudo ln -sf /sbin/hoshizora /sbin/init
```

### Recovery plan (do this BEFORE rebooting)

If hoshizora fails to boot, the kernel panics. Have a recovery path ready:

1. **Keep the old init available.** Don't delete systemd/openrc/SysV init.
   `init=/sbin/hoshizora` just adds a new option; the old `/sbin/init`
   symlink still works if you remove `init=` from the cmdline.

2. **Test the recovery path first.** Add `init=/bin/bash` to your kernel
   cmdline in GRUB (edit at boot time: press `e` on the GRUB entry, add
   `init=/bin/bash` to the `linux` line, Ctrl-X to boot). Verify you get
   a root shell with root mounted RO. This is your escape hatch.

3. **Have a live USB ready.** If GRUB itself is broken, boot from USB,
   mount the root partition, fix `/etc/hoshizora/system.hs` or remove
   `init=/sbin/hoshizora` from grub config.

### What to expect on first boot

1. Kernel boot messages
2. Hoshizora log lines on the console (stderr → console for PID 1)
3. Services start in dependency order:
   - `mount-root` remounts `/` RW
   - `mount-all` mounts everything in `/etc/fstab`
   - `hostname` sets the hostname
   - `getty-tty1` opens tty1 and prints the login prompt
4. Login as root (no password by default — set one with `passwd` after first boot)

### What doesn't work out of the box

Hoshizora is a supervisor, not a full system. You need to add services for:

- **Networking** — `dhclient eth0` or NetworkManager as a service
- **D-Bus** — `dbus-daemon --system` if any service needs it
- **Display manager** — GDM/SDDM/LightDM as a service for graphical login
- **Cron** — use Hoshizora's `every:` field on a service, or run fcron as a service
- **Logging** — `hzlog` is included; add it as a service:
  ```hcl
  service hzlog {
      exec: "/usr/bin/hzlog";
      respawn: backoff(max = 5);
  }
  ```

### Troubleshooting

**"kernel panic: Attempted to kill init!"**

Hoshizora exited. Boot with `init=/bin/bash`, then run `/sbin/hoshizora`
manually to see the error. Common causes:
- Config parse error (check `/etc/hoshizora/system.hs` syntax)
- `exec:` path doesn't exist (check binary paths)
- `start-condition:` always false (service stays STOPPED, hoshizora idle-exits if no services running)

**Boot hangs after "hoshizora starting, config=..."**

Hoshizora is running but no service is providing a console. Check that
`getty-tty1` is in your config and its `exec:` path exists. Boot with
`init=/bin/bash` and verify `/sbin/agetty` is installed.

**Login prompt appears but keyboard doesn't work**

Kernel console driver issue, not hoshizora. Check `console=tty1` vs
`console=ttyS0` in kernel cmdline. For QEMU: use `-nographic` and
`console=ttyS0`, then `agetty ttyS0` in your config.

**Services don't start in the right order**

Add `requires:` to enforce ordering. Hoshizora starts deps recursively
before the dependent. If A `requires: [B]`, B starts first; if B fails,
A doesn't start.

---

## 5. What Hoshizora does NOT do

Hoshizora is PID 1. PID 1's job is to supervise processes, not to be:

- **A login manager** — `getty`/`agetty` does that. Hoshizora starts it.
- **A network manager** — NetworkManager, dhclient, systemd-networkd do that. Hoshizora starts one of them.
- **A device manager** — udev/eudev does that. Hoshizora starts it.
- **A logind** — `hz-session` (the pam_exec helper) does a minimal version. Real logind is 50k LOC; we don't.
- **A display manager** — GDM/SDDM/LightDM do that. Hoshizora starts one.
- **A journal** — `hzlog` writes to `/var/log/messages`. Use `logrotate` for rotation.
- **A cron daemon** — Hoshizora's `every:` field covers interval scheduling. For cron-syntax `0 3 * * *`, run fcron as a service.

This is by design. systemd conflates PID 1 with all of the above. Hoshizora
is just PID 1. Everything else is a service that Hoshizora starts.

---

## 6. Quick reference

### Common commands (SOV — Subject Object Verb)

```bash
hzctl list                       # all services + state
hzctl <name> start               # start a service
hzctl <name> stop                # stop a service
hzctl <name> restart             # stop + start
hzctl <name> status              # one service's status
hzctl <name> reload              # SIGHUP the service
hzctl <name> enable              # mark for autostart (ephemeral)
hzctl <name> disable             # skip at boot/reload (ephemeral)
hzctl <target> start             # start all services in a target
hzctl logs [N]                   # last N log lines (default 50)
hzctl shutdown                   # power off (refuses if sessions open)
hzctl shutdown --force           # override session gate
hzctl reboot                     # reboot
hzctl help
```

### Environment variables

| Variable         | Default                          | Purpose                              |
|------------------|----------------------------------|--------------------------------------|
| `HZ_CTL_PATH`    | `/run/hoshizora/control`         | Control socket path                  |
| `HZ_EVENT_PATH`  | `/run/hoshizora/events`          | Plugin event socket path             |
| `HZ_NOTIFY_PATH` | `/run/hoshizora/notify`          | sd-notify socket path                |
| `HZ_SESSION_DIR` | `/run/hoshizora/sessions`        | Session files dir (shutdown gate)    |
| `HZ_LOG_SOCK`    | `/dev/log`                       | hzlog syslog socket                  |
| `HZ_LOG_FILE`    | `/var/log/messages`              | hzlog output file                    |
| `HZ_SESSION_DEVS`| `/dev/dri/* /dev/snd/* /dev/input/event* /dev/video*` | Devices for hz-session ACLs |

### Config grammar (subset)

```hcl
$VAR = "value";                    # top-level variable, substituted in string fields

system "my-host" {
    service NAME {
        exec: "/path/to/binary";
        exec: "/path/to/binary" with args ["arg1", "arg2"];
        requires: [dep1, dep2, network_ready];   # network_ready is a virtual intent
        respawn: backoff(max = 5);
        memory-limit: 256MiB;
        cpu-weight: 50;
        io-weight: 100;             # v2.2
        memory-high: 200MiB;        # v2.2 soft limit
        cpu-max: "50%";             # v2.2 quota
        oom-kill: group;
        log: "/var/log/NAME.log";
        start-condition: file-exists("/path") and link-up("eth0");
        healthy: tcp-probe("127.0.0.1:80", 5s);
        on-fail: restart(other) | shutdown;
        every: "1h";                # cron-style interval
        timeout-start: "30s";
        retry-after: "5s";
        expect-notify: true;
        listen: "0.0.0.0:80";       # socket activation
        namespace: private;         # container integration
        rootfs: "/path";
        bind: "src:dst";
        rootfs-readonly: true;
        no-new-privs: true;
        run-as: "1000:1000";
        pre-start: "cmd";
        post-stop: "cmd";
        watchdog-timeout: "30s";
        environment: { "KEY": "VALUE", "KEY2": "VALUE2" };
    }

    watch "/path" [recursive] {
        on-change: reload(NAME) | restart(NAME);
    }

    target NAME {
        requires: [svc1, svc2];
    }
}
```

---

## 7. Next steps

- Read `README.md` for the full feature list
- Read `ROADMAP.md` for what's shipped vs deferred
- Read `DEFERRED.md` for the complete ledger of `deferred:` markers in source
- Run `make test` after every change
- Use `tests/qemu.sh` for end-to-end boot testing
- Submit issues at https://github.com/Rui-727/Hoshizora/issues
