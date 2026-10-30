#!/bin/sh

set -e

TOPDIR=`pwd`
BUILD=$TOPDIR/build/host
JOBS=`getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4`

./dist-build/deps.sh

sodium_src=`cat $TOPDIR/build/src/libsodium/.vigil-fetched`

if [ ! -f $BUILD/libsodium/lib/libsodium.a ] ||
    [ "`cat $BUILD/libsodium/.vigil-fetched 2>/dev/null`" != "$sodium_src" ]; then
	rm -rf $BUILD/libsodium-src $BUILD/libsodium
	mkdir -p $BUILD
	cp -R $TOPDIR/build/src/libsodium $BUILD/libsodium-src

	(cd $BUILD/libsodium-src && \
	    ./configure --prefix=$BUILD/libsodium \
	    --disable-shared --with-pic > /dev/null && \
	    make -j$JOBS > /dev/null && make install > /dev/null)
	echo "$sodium_src" > $BUILD/libsodium/.vigil-fetched
fi

./dist-build/libkyrka.sh host LIBSODIUM_PATH=$BUILD/libsodium
./dist-build/opus.sh host
