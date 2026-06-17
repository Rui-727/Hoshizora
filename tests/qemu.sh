#!/bin/bash
# tests/qemu.sh — boot hoshizora in QEMU with a minimal initramfs.
#
# Prereqs (Debian/Ubuntu):
#   sudo apt install qemu-system-x86 busybox-static linux-image-amd64 cpio gzip
#
# What this does:
#   1. Builds hoshizora (static)
#   2. Creates a minimal initramfs with hoshizora as /init + busybox + config
#   3. Boots it in QEMU with the host kernel
#   4. You get a login prompt (or kernel panic if something's broken)
#
# To exit QEMU: Ctrl-A then X

set -e
cd "$(dirname "$0")/.."

# Find the host kernel
KERNEL=$(ls /boot/vmlinuz-* 2>/dev/null | sort -V | tail -1)
if [ -z "$KERNEL" ]; then
    echo "FAIL: no kernel found in /boot/vmlinuz-*"
    echo "  Debian/Ubuntu: sudo apt install linux-image-amd64"
    exit 1
fi
echo "[info] kernel: $KERNEL"

# Build hoshizora if needed
if [ ! -x ./hoshizora ]; then
    echo "[info] building hoshizora..."
    make
fi

# Find busybox
BUSYBOX=$(which busybox 2>/dev/null || echo "/usr/bin/busybox")
if [ ! -x "$BUSYBOX" ]; then
    echo "FAIL: busybox not found"
    echo "  Debian/Ubuntu: sudo apt install busybox-static"
    exit 1
fi

# Create initramfs
WORKDIR=$(mktemp -d)
INITRAMFS=/tmp/hz-initramfs.cpio.gz
trap 'rm -rf "$WORKDIR"' EXIT

echo "[info] building initramfs in $WORKDIR..."

# Directory structure
mkdir -p "$WORKDIR"/{bin,sbin,etc/hoshizora,dev,proc,sys,run/hoshizora,root,tmp}
chmod 1777 "$WORKDIR/tmp"

# Binaries — hoshizora as /init (kernel's default init path)
cp ./hoshizora "$WORKDIR/init"
chmod +x "$WORKDIR/init"

# busybox + symlinks for shell + coreutils
cp "$BUSYBOX" "$WORKDIR/bin/busybox"
for cmd in sh mount umount hostname ip agetty login ls cat echo mkdir rmdir \
           ln rm cp mv ps kill sleep date uname dmesg grep sed awk vi; do
    ln -s busybox "$WORKDIR/bin/$cmd"
done

# Config — use the bootable example
cp examples/system.bootable.hs "$WORKDIR/etc/hoshizora/system.hs"

# /etc/fstab — minimal. Kernel mounts root RO; hoshizora remounts RW.
cat > "$WORKDIR/etc/fstab" << 'EOF'
/dev/sda / ext4 defaults 0 1
proc /proc proc defaults 0 0
sysfs /sys sysfs defaults 0 0
devtmpfs /dev devtmpfs defaults 0 0
EOF

# /etc/passwd + /etc/shadow for login
cat > "$WORKDIR/etc/passwd" << 'EOF'
root:x:0:0:root:/root:/bin/sh
EOF
cat > "$WORKDIR/etc/shadow" << 'EOF'
root::0:0:99999:7:::
EOF
cat > "$WORKDIR/etc/group" << 'EOF'
root:x:0:
EOF

# /etc/hostname
echo "hoshizora-box" > "$WORKDIR/etc/hostname"

# Build the cpio archive
cd "$WORKDIR"
find . | cpio -o -H newc 2>/dev/null | gzip > "$INITRAMFS"
cd - > /dev/null
echo "[info] initramfs: $INITRAMFS ($(du -h "$INITRAMFS" | cut -f1))"

# Boot in QEMU
echo "[info] starting QEMU..."
echo "[info] login: root (no password)"
echo "[info] to exit QEMU: Ctrl-A then X"
echo ""

exec qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0 init=/init panic=-1" \
    -nographic \
    -m 512M \
    -no-reboot
