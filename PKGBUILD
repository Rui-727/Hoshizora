# Maintainer: Rui-727 <a192.47.72x@gmail.com>
# Contributor: Rui-727 <a192.47.72x@gmail.com>
#
# Hoshizora is a minimal PID 1 init system for Linux/x86_64. Single C file,
# libc only, static binary. This package installs the init binary plus the
# hzctl control client, hzlog syslog collector, hz-event-logger example
# plugin, the hz-session pam_exec helper, and bash/zsh completions.
#
# The package conflicts with systemd / openrc / runit because they all
# provide `/sbin/init`. Pick one init per box.

pkgname=hoshizora
pkgver=0.1.0
pkgrel=1
pkgdesc="Minimal PID 1 init system for Linux/x86_64. Single C file, libc only, static binary."
arch=('x86_64')
url="https://github.com/Rui-727/Hoshizora"
license=('MIT')
depends=('glibc')
makedepends=('git' 'gcc' 'make')
optdepends=(
    'bash-completion: hzctl tab completion'
    'pam: hz-session login tracking via pam_exec'
)
provides=('init')
conflicts=('systemd' 'openrc' 'runit' 'sinit' 'busybox-init')
source=("git+https://github.com/Rui-727/Hoshizora.git")
sha256sums=('SKIP')

build() {
    cd "$srcdir/hoshizora"
    # CC auto-detects musl-gcc if installed (85% smaller binaries), else gcc.
    # Force glibc with `make CC=gcc` by overriding in PKGBUILD if musl causes
    # issues on a given arch.
    make
}

check() {
    cd "$srcdir/hoshizora"
    # 83 tests across 9 self-checks (parse, fork+exec, deps, cgroup, fanotify,
    # start-cond, healthy, on-fail, cron, hzlog, v2.0/v2.1 features, plugin,
    # edge cases). Static binaries + bash + python3 needed.
    make test
}

package() {
    cd "$srcdir/hoshizora"
    # PREFIX=/usr installs binaries to /usr/bin, header to /usr/include,
    # scripts to /usr/lib/hoshizora, completions to /usr/share. SBINDIR
    # stays /sbin so /sbin/init symlink works for bootloaders.
    make DESTDIR="$pkgdir" PREFIX=/usr install
    # LICENSE + README ship to /usr/share/licenses for distro policy.
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
    install -Dm644 GUIDE.md "$pkgdir/usr/share/doc/$pkgname/GUIDE.md"

    # Convenience symlink: /sbin/init -> /sbin/hoshizora. Most bootloaders
    # hardcode /sbin/init. Distributions that prefer a different init can
    # remove this symlink post-install; the conflict declarations above
    # ensure only one package owns it.
    ln -sf /sbin/hoshizora "$pkgdir/sbin/init"
}
