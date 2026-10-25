#!/bin/sh

set -e

. ./dist-build/android-sdk.sh
. ./dist-build/flutter.sh

cd app
$FLUTTER build apk "$@"
