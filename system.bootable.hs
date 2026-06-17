# Hoshizora bootable config — minimal system that drops to a login prompt.
# This is what you'd put at /etc/hoshizora/system.hs on a real install.
#
# After boot: hoshizora starts these services in dep order, then you see
# a login prompt on tty1 (and ttyS0 if booted with console=ttyS0).

system "bootable" {
    # Mount filesystems from /etc/fstab. Must succeed or nothing else works.
    # deferred: no fsck pass — add a fsck service before this if your fstab
    # has root errors. Real distros run fsck first; we assume the kernel
    # already mounted root read-only and we remount RW here.
    service mount-root {
        exec: "/bin/mount" with args ["-o", "remount,rw", "/"];
        on-fail: shutdown;
    }

    service mount-all {
        exec: "/bin/mount" with args ["-a"];
        requires: [mount-root];
        on-fail: shutdown;
    }

    # udev coldplug — populate /dev with permissions. If you don't have udev,
    # the kernel's devtmpfs already covers basic device nodes.
    service udev {
        exec: "/usr/lib/systemd/systemd-udevd";
        requires: [mount-all];
        respawn: backoff(max = 5);
        # deferred: no udevadm trigger — add a oneshot service for coldplug
        # if your distro needs it. Most modern kernels auto-populate.
    }

    # Set hostname
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

    # Getty on tty1 — THIS is what gives you a login prompt on the console.
    # agetty opens tty1, prints "login:", hands off to /bin/login.
    # respawn so if you exit the shell, you get a new login prompt.
    service getty-tty1 {
        exec: "/sbin/agetty" with args ["tty1", "linux"];
        requires: [mount-all, hostname];
        respawn: backoff(max = 5);
    }

    # Getty on serial console — for QEMU -nographic or real serial (IPMI, etc.)
    # Comment out if you don't have a serial console.
    service getty-ttyS0 {
        exec: "/sbin/agetty" with args ["ttyS0", "115200", "linux"];
        requires: [mount-all, hostname];
        respawn: backoff(max = 5);
    }

    # Optional: DHCP on eth0. Comment out if you don't have dhclient.
    # service dhcp-eth0 {
    #     exec: "/sbin/dhclient" with args ["eth0"];
    #     requires: [net-lo];
    #     respawn: backoff(max = 3);
    # }
}
