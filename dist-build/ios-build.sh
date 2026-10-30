#!/bin/sh

set -e

TOPDIR=`pwd`
OUT=$TOPDIR/build/ios
IOS_MIN=18.0

./dist-build/deps.sh

mkdir -p $OUT

sodium_src="`cat $TOPDIR/build/src/libsodium/.vigil-fetched` $IOS_MIN"

if [ ! -d $OUT/Clibsodium.xcframework ] ||
    [ "`cat $OUT/.libsodium 2>/dev/null`" != "$sodium_src" ]; then
	rm -rf $TOPDIR/build/ios-libsodium-src
	cp -R $TOPDIR/build/src/libsodium $TOPDIR/build/ios-libsodium-src
	(cd $TOPDIR/build/ios-libsodium-src && \
	    env LIBSODIUM_FULL_BUILD=1 LIBSODIUM_SKIP_SIMULATORS= \
	    IOS_VERSION_MIN=$IOS_MIN.0 IOS_SIMULATOR_VERSION_MIN=$IOS_MIN.0 \
	    ./dist-build/apple-xcframework.sh)
	rm -rf $OUT/Clibsodium.xcframework
	cp -r $TOPDIR/build/ios-libsodium-src/libsodium-apple/Clibsodium.xcframework \
	    $OUT/
	echo "$sodium_src" > $OUT/.libsodium
fi

for target in ios-arm64 ios-simulator-arm64; do
	case $target in
	ios-arm64)
		sdk=iphoneos
		slice=ios-arm64
		cflags="-arch arm64 -mios-version-min=$IOS_MIN"
		;;
	ios-simulator-arm64)
		sdk=iphonesimulator
		slice=ios-arm64_x86_64-simulator
		cflags="-arch arm64 -mios-simulator-version-min=$IOS_MIN"
		;;
	esac

	cc="xcrun -sdk $sdk clang $cflags"
	sodium=$TOPDIR/build/$target/libsodium

	mkdir -p $sodium/include $sodium/lib
	cp -r $OUT/Clibsodium.xcframework/$slice/Headers/* $sodium/include/
	cp $OUT/Clibsodium.xcframework/$slice/libsodium.a $sodium/lib/

	./dist-build/libkyrka.sh $target \
	    OSNAME=ios CROSS_BUILD=1 CC="$cc" LIBSODIUM_PATH=$sodium

	./dist-build/opus.sh $target CC="$cc" AR=ar RANLIB=ranlib

	make clean-objs
	make PLATFORM=ios CC="$cc" KYRKA=$TOPDIR/build/$target \
	    LIBSODIUM_PATH=$sodium libvigil.a
	mv libvigil.a $TOPDIR/build/$target/lib/
	make clean-objs

	libtool -static -o $TOPDIR/build/$target/lib/libvigil-all.a \
	    $TOPDIR/build/$target/lib/libvigil.a \
	    $TOPDIR/build/$target/lib/libkyrka.a \
	    $TOPDIR/build/$target/opus/lib/libopus.a \
	    $sodium/lib/libsodium.a
done

rm -rf $OUT/Vigil.xcframework

mkdir -p $OUT/headers
cp include/vigil_ios.h $OUT/headers/
cp include/vigil_shared.h $OUT/headers/
cat > $OUT/headers/module.modulemap << _EOF
module Vigil {
	header "vigil_ios.h"
	link framework "AudioToolbox"
	export *
}
_EOF

xcodebuild -create-xcframework \
    -library build/ios-arm64/lib/libvigil-all.a -headers $OUT/headers \
    -library build/ios-simulator-arm64/lib/libvigil-all.a \
    -headers $OUT/headers \
    -output $OUT/Vigil.xcframework
