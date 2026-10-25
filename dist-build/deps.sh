#!/bin/sh

set -e

TOPDIR=`pwd`
SRC=$TOPDIR/build/src

. ./dist-build/fetch.sh

LIBKYRKA_REV=17b52d2e95d7e0aa8e02a8431484130f59fbd197
LIBKYRKA_URL=https://github.com/jorisvink/libkyrka/archive/$LIBKYRKA_REV.tar.gz
LIBKYRKA_SHA256=80a7def87b85d097c1f7a76868a69294f6531cde9f18a4e94291a26a3a67912c

# libsodium, used by libkyrka for x25519, ed25519 and (on hosts) AES-GCM.
LIBSODIUM_VERSION=1.0.20
LIBSODIUM_URL=https://download.libsodium.org/libsodium/releases/libsodium-$LIBSODIUM_VERSION.tar.gz
LIBSODIUM_SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19

# mbedtls, used by libkyrka for AES-GCM on Android.
MBEDTLS_VERSION=3.6.7
MBEDTLS_URL=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2
MBEDTLS_SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

# opus, used for the voice in calls.
OPUS_VERSION=1.5.2
OPUS_URL=https://downloads.xiph.org/releases/opus/opus-$OPUS_VERSION.tar.gz
OPUS_SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1

fetch libkyrka $LIBKYRKA_URL $LIBKYRKA_SHA256 \
    libkyrka-$LIBKYRKA_REV.tar.gz $SRC
fetch libsodium $LIBSODIUM_URL $LIBSODIUM_SHA256 \
    libsodium-$LIBSODIUM_VERSION.tar.gz $SRC
fetch mbedtls $MBEDTLS_URL $MBEDTLS_SHA256 \
    mbedtls-$MBEDTLS_VERSION.tar.bz2 $SRC
fetch opus $OPUS_URL $OPUS_SHA256 \
    opus-$OPUS_VERSION.tar.gz $SRC

# libkyrka wants a git checkout or a RELEASE file for its version.
if [ ! -f $SRC/libkyrka/RELEASE ]; then
	echo "master-`echo $LIBKYRKA_REV | cut -c1-8`" > $SRC/libkyrka/RELEASE
fi
