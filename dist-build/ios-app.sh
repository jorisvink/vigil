#!/bin/sh

set -e

. ./dist-build/flutter.sh

cd app
$FLUTTER build ios "$@"
