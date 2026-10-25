# Pulled in by the other dist-build scripts.

FLUTTER_VERSION=3.47.7
FLUTTER_BASE=https://storage.googleapis.com/flutter_infra_release/releases

if [ -z "$FLUTTER" ] && command -v flutter > /dev/null; then
	FLUTTER=`command -v flutter`
fi

if [ -z "$FLUTTER" ]; then
	case `uname -s`-`uname -m` in
	Linux-x86_64)
		archive=flutter_linux_$FLUTTER_VERSION-stable.tar.xz
		url=$FLUTTER_BASE/stable/linux/$archive
		hash=af7455f540df951184c7953fbe3cf884766e7541dacf8571f9f582db03dd6230
		;;
	Darwin-arm64)
		archive=flutter_macos_arm64_$FLUTTER_VERSION-stable.zip
		url=$FLUTTER_BASE/stable/macos/$archive
		hash=dd9da574c3168460550117eb0e8dd0313ae934fabf64a82a6d557fff5fd1422e
		;;
	Darwin-x86_64)
		archive=flutter_macos_$FLUTTER_VERSION-stable.zip
		url=$FLUTTER_BASE/stable/macos/$archive
		hash=17d39b000a5d0681e3f7b61a105a36f6f31dd41ab9073f625fd466f38035f901
		;;
	*)
		echo "no flutter found and no release to fetch for this host,"
		echo "install flutter $FLUTTER_VERSION and put it on the PATH"
		exit 1
		;;
	esac

	. ./dist-build/fetch.sh
	fetch flutter $url $hash $archive `pwd`/build/tools

	FLUTTER=`pwd`/build/tools/flutter/bin/flutter
fi

echo "using flutter $FLUTTER"
