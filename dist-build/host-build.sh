#!/bin/sh

set -e

TOPDIR=`pwd`
BUILD=$TOPDIR/build/host

./dist-build/deps.sh

./dist-build/libsodium.sh host

./dist-build/libkyrka.sh host LIBSODIUM_PATH=$BUILD/libsodium
./dist-build/opus.sh host
