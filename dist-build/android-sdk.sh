# Pulled in by the other dist-build scripts.

SDK=${ANDROID_HOME:-$ANDROID_SDK_ROOT}

if [ -z "$SDK" ] && [ -f app/android/local.properties ]; then
	SDK=`sed -n 's/^sdk\.dir=//p' app/android/local.properties`
fi

if [ -z "$SDK" ]; then
	for dir in $HOME/Android/Sdk $HOME/Library/Android/sdk; do
		if [ -d $dir ]; then
			SDK=$dir
			break
		fi
	done
fi

# Pointing at ~/Android instead of ~/Android/Sdk is an easy mistake.
if [ -n "$SDK" ] && [ ! -d "$SDK/platform-tools" ] &&
    [ ! -d "$SDK/platforms" ] && [ -d "$SDK/Sdk" ]; then
	SDK=$SDK/Sdk
fi

if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
	echo "no android sdk found, set ANDROID_HOME to it (eg: ~/Android/Sdk)"
	exit 1
fi

ndk_fetch() {
	NDK_RELEASE=r28c

	case `uname -s` in
	Linux)
		ndk_host=linux
		ndk_sha1=a7b54a5de87fecd125a17d54f73c446199e72a64
		;;
	Darwin)
		ndk_host=darwin
		ndk_sha1=fc20a6bf15a30fb3428c9b60a7308793a362dc6d
		;;
	*)
		echo "no android ndk $NDK_VERSION found and none to fetch"
		exit 1
		;;
	esac

	ndk_zip=android-ndk-$NDK_RELEASE-$ndk_host.zip

	. ./dist-build/fetch.sh
	fetch android-ndk https://dl.google.com/android/repository/$ndk_zip \
	    $ndk_sha1 $ndk_zip `pwd`/build/tools
}

NDK=${ANDROID_NDK_HOME:-$ANDROID_NDK_ROOT}

if [ -n "$NDK" ]; then
	if [ ! -f "$NDK/source.properties" ]; then
		echo "ANDROID_NDK_HOME ($NDK) is not an android ndk"
		exit 1
	fi
	NDK_VERSION=`sed -n 's/^Pkg\.Revision *= *//p' $NDK/source.properties`
	export ANDROID_NDK_HOME=$NDK
else
	NDK_VERSION=`sed -n 's/.*pinnedNdk = "\(.*\)".*/\1/p' \
	    app/android/app/build.gradle.kts`
	NDK=$SDK/ndk/$NDK_VERSION

	# A fetched NDK of this version wins, as it does for Gradle.
	fetched=
	if [ -f build/tools/android-ndk/source.properties ]; then
		fetched=`sed -n 's/^Pkg\.Revision *= *//p' \
		    build/tools/android-ndk/source.properties`
	fi

	if [ "$fetched" = "$NDK_VERSION" ] ||
	    [ ! -f $NDK/source.properties ]; then
		ndk_fetch
		NDK=`pwd`/build/tools/android-ndk
		export ANDROID_NDK_HOME=$NDK
	fi
fi

if [ -z "$JAVA_HOME" ] && ! command -v java > /dev/null; then
	for dir in "/Applications/Android Studio.app/Contents/jbr/Contents/Home" \
	    /opt/android-studio/jbr $HOME/android-studio/jbr \
	    /snap/android-studio/current/jbr \
	    /usr/local/android-studio/jbr; do
		if [ -x "$dir/bin/java" ]; then
			export JAVA_HOME=$dir
			break
		fi
	done
fi

export ANDROID_HOME=$SDK
unset ANDROID_SDK_ROOT ANDROID_NDK_ROOT
