#!/bin/sh

set -e

TOPDIR=`pwd`
OUT=$TOPDIR/build/ios
IOS_MIN=18.0

./dist-build/deps.sh

mkdir -p $OUT

for target in ios-arm64 ios-simulator-arm64; do
	case $target in
	ios-arm64)
		sdk=iphoneos
		cflags="-arch arm64 -mios-version-min=$IOS_MIN"
		;;
	ios-simulator-arm64)
		sdk=iphonesimulator
		cflags="-arch arm64 -mios-simulator-version-min=$IOS_MIN"
		;;
	esac

	cc="xcrun -sdk $sdk clang $cflags"
	sodium=$TOPDIR/build/$target/libsodium

	./dist-build/libsodium.sh $target \
	    --host=aarch64-apple-darwin CC="$cc"

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
