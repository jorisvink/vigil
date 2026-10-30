#!/bin/sh

set -e

if [ -z "$1" ]; then
	echo "usage: libsodium.sh <target> [configure arguments]"
	exit 1
fi

TARGET=$1
shift

TOPDIR=`pwd`
BUILD=$TOPDIR/build/$TARGET
SRC=$TOPDIR/build/src/libsodium
JOBS=`getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4`

if [ ! -f $SRC/src/libsodium/include/sodium.h ]; then
	echo "no libsodium found at $SRC, run dist-build/deps.sh"
	exit 1
fi

# Rebuild when the tarball or the build arguments change, the latter
# covers things like a bumped minimum OS version or a different CC.
stamp="`cat $SRC/.vigil-fetched` $*"

if [ -f $BUILD/libsodium/lib/libsodium.a ] &&
    [ "`cat $BUILD/libsodium/.vigil-fetched 2>/dev/null`" = "$stamp" ]; then
	exit 0
fi

rm -rf $BUILD/libsodium-src $BUILD/libsodium
mkdir -p $BUILD
cp -R $SRC $BUILD/libsodium-src

echo "building libsodium for $TARGET"
(cd $BUILD/libsodium-src && \
    ./configure --prefix=$BUILD/libsodium \
    --disable-shared --with-pic "$@" > /dev/null && \
    make -j$JOBS > /dev/null && make install > /dev/null)

echo "$stamp" > $BUILD/libsodium/.vigil-fetched
