# Maintainer: CtrlAltLilith
# Contributor: Chih-Hsuan Yen <yan12125@archlinux.org>
# Contributor: Florian pritz <bluewind@xinu.at>
# Contributor: Bartłomiej Piotrowski <nospam@bpiotrowski.pl>
# Contributor: Brad Fanella <bradfanella@archlinux.us>
# Contributor: Andrea Scarpino <andrea@archlinux.org>
# Contributor: tobias <tobias@archlinux.org>
# This script has been modified from the arch repos
pkgname=openbox-lilith
pkgver=3.70git
pkgrel=1
pkgdesc='Openbox fork with major patches and default aesthetic changes'
arch=('x86_64')
url='https://github.com/CtrlAltLilith/openbox'
license=('GPL')
provides=(openbox libobrender.so)
conflicts=(openbox)
depends=('startup-notification' 'libxml2' 'libxinerama' 'libxrandr'
         'libxcursor' 'pango' 'imlib2' 'librsvg' 'libsm')
makedepends=('python')
optdepends=('plasma-workspace: for the KDE/Openbox xsession'
            'python-xdg: for the openbox-xdg-autostart script')
groups=('lxde' 'lxde-gtk3' 'lxqt')
backup=('etc/xdg/openbox/menu.xml' 'etc/xdg/openbox/rc.xml'
        'etc/xdg/openbox/autostart' 'etc/xdg/openbox/environment')
source=(https://github.com/CtrlAltLilith/openbox/archive/master.tar.gz)
md5sums=('SKIP')

build() {
    cd "openbox-master"
./bootstrap
./configure --prefix=/usr --sysconfdir=/etc
  make
}

package() {
    cd "openbox-master"
  make DESTDIR="$pkgdir" install

  # GNOME Panel is no longer available in the official repositories
  rm -r "$pkgdir"/usr/bin/{gdm-control,gnome-panel-control,openbox-gnome-session} \
    "$pkgdir"/usr/share/gnome{,-session} \
    "$pkgdir"/usr/share/man/man1/openbox-gnome-session.1 \
    "$pkgdir"/usr/share/xsessions/openbox-gnome.desktop
}
