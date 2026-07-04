# Hoshizora bootable config, minimal system that drops to a login prompt.
# This is what you'd put at /etc/hoshizora/system.hs on a real install.
#
# Boot sequence follows the pattern from dinit's DINIT-AS-INIT.md:
# early filesystems → udev → hwclock → rootfs remount → mount-all →
# hostname → net-lo → syslog → getty.
#
# See GUIDE.md section 4.3 for the LFS-tuned version of this config
# (includes early-mounts, udev-trigger, hwclock, sysklogd).

system "bootable" {
    # Early virtual filesystems. initramfs should already mount these,
    # but mount them here as a safety net.
    service early-mounts {
        exec: "/bin/sh" with args ["-c", "mount -t proc proc /proc 2>/dev/null; mount -t sysfs sysfs /sys 2>/dev/null; mount -t devtmpfs devtmpfs /dev 2>/dev/null; mount -t tmpfs tmpfs /run 2>/dev/null; true"];
        on-fail: shutdown;
    }

    # udev, device node manager. Needs /sys and /dev already mounted.
    # Path varies by distro: /usr/lib/systemd/systemd-udevd (Arch),
    # /sbin/udevd (LFS-OpenRC), /usr/lib/udev/udevd (older). Adjust.
    service udev {
        exec: "/usr/lib/systemd/systemd-udevd";
        requires: [early-mounts];
        respawn: backoff(max = 5);
    }

    # Coldplug, process devices that existed before udevd started
    service udev-trigger {
        exec: "/usr/bin/udevadm" with args ["trigger", "--action=add"];
        requires: [udev];
    }

    # Set system clock from hardware RTC
    service hwclock {
        exec: "/sbin/hwclock" with args ["--hctosys"];
        requires: [udev-trigger];
    }

    # Remount root RW. Kernel mounts root RO; we need RW for logs, etc.
    service mount-root {
        exec: "/bin/mount" with args ["-o", "remount,rw", "/"];
        requires: [hwclock];
        on-fail: shutdown;
    }

    # Mount everything in /etc/fstab
    service mount-all {
        exec: "/bin/mount" with args ["-a"];
        requires: [mount-root];
        on-fail: shutdown;
    }

    # Hostname
    service hostname {
        exec: "/bin/hostname" with args ["hoshizora-box"];
        requires: [mount-all];
    }

    # Loopback networking
    service net-lo {
        exec: "/sbin/ip" with args ["link", "set", "lo", "up"];
        requires: [mount-all];
        on-fail: shutdown;
    }

    # Getty on tty1. THIS is what gives you a login prompt on the console.
    # agetty opens tty1, prints "login:", hands off to /bin/login.
    # respawn so if you exit the shell, you get a new login prompt.
    service getty-tty1 {
        exec: "/sbin/agetty" with args ["tty1", "linux"];
        requires: [mount-all, hostname, udev-trigger];
        respawn: backoff(max = 5);
    }

    # Getty on serial console, for QEMU -nographic or real serial (IPMI, etc.)
    service getty-ttyS0 {
        exec: "/sbin/agetty" with args ["ttyS0", "115200", "linux"];
        requires: [mount-all, hostname, udev-trigger];
        respawn: backoff(max = 5);
    }

    # Hoshizora's own log collector, collects /dev/log, writes to
    # /var/log/messages. Override paths via HZ_LOG_SOCK / HZ_LOG_FILE.
    service hzlog {
        exec: "/usr/bin/hzlog";
        requires: [mount-all];
        respawn: backoff(max = 5);
    }

    # Optional: DHCP on eth0. Uncomment if you have dhclient.
    # service dhcp-eth0 {
    #     exec: "/sbin/dhclient" with args ["eth0"];
    #     requires: [net-lo];
    #     respawn: backoff(max = 3);
    # }
}
