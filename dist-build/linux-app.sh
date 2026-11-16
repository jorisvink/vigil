#!/bin/sh

set -e

. ./dist-build/flutter.sh

[ -f build/host/lib/libkyrka.a ] || ./dist-build/host-build.sh
make

cd app
$FLUTTER build linux "$@"
