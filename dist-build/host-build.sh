#!/bin/sh

set -e

TOPDIR=`pwd`
BUILD=$TOPDIR/build/host
JOBS=`getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4`

./dist-build/deps.sh

if [ ! -f $BUILD/libsodium/lib/libsodium.a ]; then
	rm -rf $BUILD/libsodium-src $BUILD/libsodium
	mkdir -p $BUILD
	cp -R $TOPDIR/build/src/libsodium $BUILD/libsodium-src

	(cd $BUILD/libsodium-src && \
	    ./configure --prefix=$BUILD/libsodium \
	    --disable-shared --with-pic > /dev/null && \
	    make -j$JOBS > /dev/null && make install > /dev/null)
fi

./dist-build/libkyrka.sh host LIBSODIUM_PATH=$BUILD/libsodium
./dist-build/opus.sh host
