#!/bin/sh

set -e

TOPDIR=`pwd`
API=28
JOBS=`getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4`

./dist-build/deps.sh

. ./dist-build/android-sdk.sh

HOST=`uname -s | tr '[:upper:]' '[:lower:]'`-x86_64
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/$HOST/bin

if [ ! -d $TOOLCHAIN ]; then
	echo "no android ndk $NDK_VERSION found at $NDK"
	echo "point ANDROID_NDK_HOME at a standalone ndk"
	exit 1
fi

echo "using android sdk $SDK"
echo "using android ndk $NDK ($NDK_VERSION)"

for abi in arm64-v8a x86_64; do
	case $abi in
	arm64-v8a)
		script=android-armv8-a.sh
		sodium=libsodium-android-armv8-a+crypto
		cc=aarch64-linux-android$API-clang
		;;
	x86_64)
		script=android-x86_64.sh
		sodium=libsodium-android-westmere
		cc=x86_64-linux-android$API-clang
		;;
	esac

	BUILD=$TOPDIR/build/android-$abi

	# Everything for an ABI must be built with the same NDK.
	if [ -f $BUILD/.ndk ] && [ "`cat $BUILD/.ndk`" != "$NDK" ]; then
		echo "ndk changed for $abi, rebuilding its dependencies"
		rm -rf $BUILD
	fi

	mkdir -p $BUILD
	echo "$NDK" > $BUILD/.ndk

	sodium_src=`cat $TOPDIR/build/src/libsodium/.vigil-fetched`

	if [ ! -f $BUILD/libsodium/lib/libsodium.a ] ||
	    [ "`cat $BUILD/libsodium/.vigil-fetched 2>/dev/null`" != "$sodium_src" ]; then
		rm -rf $BUILD/libsodium-src $BUILD/libsodium
		mkdir -p $BUILD
		cp -R $TOPDIR/build/src/libsodium $BUILD/libsodium-src

		(cd $BUILD/libsodium-src && \
		    env ANDROID_NDK_HOME=$NDK NDK_PLATFORM=android-$API \
		    LIBSODIUM_FULL_BUILD=1 ./dist-build/$script)

		cp -R $BUILD/libsodium-src/$sodium $BUILD/libsodium
		echo "$sodium_src" > $BUILD/libsodium/.vigil-fetched
	fi

	if [ ! -f $BUILD/mbedtls/lib/libmbedcrypto.a ]; then
		src=$TOPDIR/build/src/mbedtls
		rm -rf $BUILD/mbedtls-build $BUILD/mbedtls
		mkdir -p $BUILD/mbedtls-build $BUILD/mbedtls/lib

		files=`sed -n '/^set(src_crypto/,/)/p' \
		    $src/library/CMakeLists.txt | grep '\.c$'`

		echo "building mbedcrypto for $abi"
		n=0
		for f in $files; do
			obj=$BUILD/mbedtls-build/`basename $f .c`.o
			$TOOLCHAIN/$cc -O2 -fPIC -I$src/include -I$src/library \
			    -c $src/library/$f -o $obj &
			n=`expr $n + 1`
			if [ $n -ge $JOBS ]; then
				wait
				n=0
			fi
		done
		wait

		$TOOLCHAIN/llvm-ar rcs $BUILD/mbedtls/lib/libmbedcrypto.a \
		    $BUILD/mbedtls-build/*.o
		cp -R $src/include $BUILD/mbedtls/include
	fi

	./dist-build/opus.sh android-$abi \
	    CC=$TOOLCHAIN/$cc AR=$TOOLCHAIN/llvm-ar RANLIB=$TOOLCHAIN/llvm-ranlib

	env CFLAGS="-I$BUILD/mbedtls/include" \
	    ./dist-build/libkyrka.sh android-$abi \
	    OSNAME=linux CROSS_BUILD=1 \
	    CC=$TOOLCHAIN/$cc AR=$TOOLCHAIN/llvm-ar \
	    CIPHER=mbedtls-aes-gcm MBEDTLS=1 \
	    LIBSODIUM_PATH=$BUILD/libsodium
done

make version

# Let flutter write app/android/local.properties so Android Studio
# can open app/android right away.
. ./dist-build/flutter.sh
(cd app && $FLUTTER pub get)
