#!/bin/sh

set -e

BUNDLE=app/build/linux/x64/release/bundle
PREFIX=${PREFIX:-$HOME/.local}

APPDIR=$PREFIX/lib/vigil
BINDIR=$PREFIX/bin
DESKTOP=$PREFIX/share/applications
ICONS=$PREFIX/share/icons/hicolor

refresh() {
	if command -v gtk-update-icon-cache > /dev/null; then
		gtk-update-icon-cache -q -f -t $ICONS || true
	fi

	if command -v update-desktop-database > /dev/null; then
		update-desktop-database -q $DESKTOP || true
	fi
}

if [ "$1" = "uninstall" ]; then
	rm -rf $APPDIR
	rm -f $BINDIR/vigil
	rm -f $DESKTOP/vigil.desktop
	rm -f $ICONS/256x256/apps/vigil.png
	refresh
	echo "removed vigil from $PREFIX"
	exit 0
fi

if [ ! -x $BUNDLE/vigil ]; then
	echo "no linux build found, run make linux first"
	exit 1
fi

mkdir -p $PREFIX/lib $BINDIR $DESKTOP $ICONS/256x256/apps

rm -rf $APPDIR
cp -R $BUNDLE $APPDIR

ln -sf $APPDIR/vigil $BINDIR/vigil

cp app/linux/runner/vigil.png $ICONS/256x256/apps/vigil.png

sed "s|@APPDIR@|$APPDIR|" app/linux/vigil.desktop \
    > $DESKTOP/vigil.desktop

refresh

echo "installed vigil into $APPDIR"
echo "added $DESKTOP/vigil.desktop"
