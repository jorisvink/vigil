#!/bin/sh

set -e

if [ -z "$1" ]; then
	echo "usage: opus.sh <target> [make variables]"
	exit 1
fi

TARGET=$1
shift

TOPDIR=`pwd`
BUILD=$TOPDIR/build/$TARGET
JOBS=`getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4`

if [ ! -f $TOPDIR/build/src/opus/include/opus.h ]; then
	echo "no opus found at $TOPDIR/build/src/opus, run dist-build/deps.sh"
	exit 1
fi

if [ -f $BUILD/opus/lib/libopus.a ]; then
	exit 0
fi

rm -rf $BUILD/opus-src $BUILD/opus
mkdir -p $BUILD/opus/lib $BUILD/opus/include/opus
cp -R $TOPDIR/build/src/opus $BUILD/opus-src

echo "building opus for $TARGET"
env CFLAGS="-fPIC -DHAVE_LRINTF $CFLAGS" \
    make -C $BUILD/opus-src -f Makefile.unix -j$JOBS "$@" lib > /dev/null

cp $BUILD/opus-src/libopus.a $BUILD/opus/lib/
cp $BUILD/opus-src/include/*.h $BUILD/opus/include/opus/
