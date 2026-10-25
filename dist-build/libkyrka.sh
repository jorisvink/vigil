#!/bin/sh

set -e

if [ -z "$1" ]; then
	echo "usage: libkyrka.sh <target> [make variables]"
	exit 1
fi

TARGET=$1
shift

TOPDIR=`pwd`
LIBKYRKA=${LIBKYRKA:-$TOPDIR/build/src/libkyrka}
BUILD=$TOPDIR/build/$TARGET
SRC=$BUILD/libkyrka

if [ ! -f $LIBKYRKA/include/libkyrka/libkyrka.h ]; then
	echo "no libkyrka found at $LIBKYRKA, run dist-build/deps.sh"
	exit 1
fi

mkdir -p $BUILD/include/libkyrka $BUILD/lib

rm -rf $SRC
cp -R $LIBKYRKA $SRC
rm -rf $SRC/.git

for p in $TOPDIR/dist-build/patches/libkyrka-*.patch; do
	[ -f "$p" ] || continue
	patch -s -p1 -d $SRC < $p
done

if [ -d $LIBKYRKA/.git ]; then
	REV=`git -C $LIBKYRKA rev-parse --short=8 HEAD`
	BRANCH=`git -C $LIBKYRKA rev-parse --abbrev-ref HEAD`
	echo "$BRANCH-$REV" > $SRC/RELEASE
elif [ ! -f $SRC/RELEASE ]; then
	echo "unknown" > $SRC/RELEASE
fi

make -C $SRC OBJDIR=obj "$@"

cp $SRC/libkyrka.a $BUILD/lib/
cp $SRC/include/libkyrka/*.h $BUILD/include/libkyrka/
cp $SRC/nyfe/include/libnyfe.h $BUILD/include/libkyrka/
cp $SRC/nyfe/include/portable_win.h $BUILD/include/libkyrka/
cp $SRC/nyfe/include/portable_esp32.h $BUILD/include/libkyrka/
