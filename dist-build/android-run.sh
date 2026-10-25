#!/bin/sh

set -e

. ./dist-build/android-sdk.sh
. ./dist-build/flutter.sh

ADB=$SDK/platform-tools/adb
EMULATOR=$SDK/emulator/emulator
AVD_HOME=${ANDROID_AVD_HOME:-$HOME/.android/avd}

avd_usable() {
	dir=`sed -n 's/^path=//p' $AVD_HOME/$1.ini 2> /dev/null`
	[ -n "$dir" ] || dir=$AVD_HOME/$1.avd

	img=`sed -n 's/^image.sysdir.1=//p' $dir/config.ini 2> /dev/null`
	[ -n "$img" ] && [ -d "$SDK/$img" ]
}

if [ ! -x $ADB ]; then
	echo "no adb found, install it with: sdkmanager platform-tools"
	exit 1
fi

$ADB start-server > /dev/null 2>&1

device=`$ADB devices | awk 'NR > 1 && $2 == "device" { print $1; exit }'`

if [ -n "$device" ]; then
	echo "using already running device $device"
	case $device in
	emulator-*)
		echo "(to boot a fresh one, first: adb -s $device emu kill)"
		;;
	esac
fi

if [ -z "$device" ]; then
	if [ ! -x $EMULATOR ]; then
		echo "no device attached and no emulator installed"
		echo "install it with: sdkmanager emulator"
		exit 1
	fi

	avd=$AVD
	if [ -z "$avd" ]; then
		for name in `$EMULATOR -list-avds 2> /dev/null`; do
			if avd_usable $name; then
				avd=$name
				break
			fi
			echo "skipping AVD $name, its system image is not installed"
		done
	fi

	if [ -z "$avd" ]; then
		echo "no device attached and no usable emulator (AVD),"
		echo "create one, for example:"
		echo
		echo "  sdkmanager \"system-images;android-35;google_apis;x86_64\""
		echo "  avdmanager create avd -n vigil -d pixel_7 \\"
		echo "      -k \"system-images;android-35;google_apis;x86_64\""
		exit 1
	fi

	flags="-no-snapshot-save -no-audio -no-boot-anim $EMULATOR_FLAGS"

	if [ "`uname -s`" = "Linux" ] && [ -z "$DISPLAY" ] &&
	    [ -z "$WAYLAND_DISPLAY" ]; then
		echo "no display found, running the emulator headless"
		flags="$flags -no-window"
	fi

	echo "booting emulator $avd"
	mkdir -p build
	$EMULATOR -avd $avd $flags > build/emulator-$avd.log 2>&1 &
	pid=$!

	while [ "`$ADB shell getprop sys.boot_completed 2> /dev/null | \
	    tr -d '\r'`" != "1" ]; do
		if ! kill -0 $pid 2> /dev/null; then
			echo "emulator $avd exited, see build/emulator-$avd.log:"
			tail -5 build/emulator-$avd.log
			exit 1
		fi
		sleep 2
	done

	device=`$ADB devices | awk 'NR > 1 && $2 == "device" { print $1; exit }'`
	echo "emulator $avd is up as $device"
fi

cd app
exec $FLUTTER run -d $device "$@"
