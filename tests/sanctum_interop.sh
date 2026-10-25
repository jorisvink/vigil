#!/bin/sh
#
# Interop test between vigil and a real sanctum tunnel.
#
# usage: tests/sanctum_interop.sh <sanctum binary> [idle seconds]
#
# sanctum needs to be able to create its tun interface and drop to the
# configured users, as non-root that means giving the binary file
# capabilities (testing only, see below):
#
#   sudo setcap cap_net_admin,cap_setuid,cap_setgid,cap_sys_admin+ep \
#       build/sanctum-test/sanctum
#
# Both sides run MTU discovery. We ping sanctum its tun address a few
# times and then go idle, without our heartbeats sanctum would declare
# us unresponsive after 120 seconds.
#
# Checks:
#   - pings through the tunnel are answered by the host behind sanctum
#   - sanctum acked one of our MTU probes (vigil logs MTU ack received)
#   - we acked one of sanctum its MTU probes (sanctum logs it)
#   - sanctum never considered us unresponsive while we were idle

set -e

if [ -z "$1" ]; then
	echo "usage: tests/sanctum_interop.sh <sanctum binary> [idle seconds]"
	exit 1
fi

SANCTUM=`realpath $1`
IDLE=${2:-150}
USER=`id -un`
PORT=4520
DIR=`mktemp -d`

cleanup() {
	[ -n "$PID" ] && kill $PID 2> /dev/null || true
	sleep 1
	rm -rf $DIR
}

trap cleanup EXIT

head -c 32 /dev/urandom > $DIR/secret
SECRET=`od -An -tx1 -v $DIR/secret | tr -d ' \n'`

cat > $DIR/sanctum.conf << EOF
instance vigiltest
secret $DIR/secret
local 127.0.0.1:$PORT
tunnel 10.66.0.1/24 auto
control $DIR/control $USER
pidfile $DIR/sanctum.pid
run control as $USER
run bless as $USER
run chapel as $USER
run confess as $USER
run heaven-rx as $USER
run heaven-tx as $USER
run purgatory-rx as $USER
run purgatory-tx as $USER
EOF

cat > $DIR/vigil.conf << EOF
vigil 1
instance sanctum
tunnel 10.66.0.2/24 auto
peer 127.0.0.1:$PORT
secret $SECRET
EOF

$SANCTUM -c $DIR/sanctum.conf > $DIR/sanctum.log 2>&1 &
PID=$!
sleep 3

if ! kill -0 $PID 2> /dev/null; then
	echo "FAIL sanctum did not start:"
	cat $DIR/sanctum.log
	exit 1
fi

echo "sanctum up, running vigil (idle for $IDLE seconds after pinging)"

if ! ./tests/vigild -p 10.66.0.1 -n 5 -w $IDLE $DIR/vigil.conf \
    > $DIR/vigil.log 2>&1; then
	echo "FAIL vigil did not get its replies"
	tail -20 $DIR/vigil.log
	exit 1
fi

strip() {
	sed 's/\x1b\[[0-9;]*m//g' $1
}

fail=0

if ! grep -q "got 5 replies" $DIR/vigil.log; then
	echo "FAIL no ping replies"
	fail=1
fi

if grep -q "MTU ack received" $DIR/vigil.log; then
	echo "ok   sanctum acked our MTU probe:" \
	    `grep -o "mtu=[0-9]*" $DIR/vigil.log | tail -1`
else
	echo "FAIL sanctum never acked our MTU probes"
	fail=1
fi

if strip $DIR/sanctum.log | grep -q "MTU ack received"; then
	echo "ok   we acked sanctum its MTU probe:" \
	    `strip $DIR/sanctum.log | grep -o "mtu=[0-9]*" | tail -1`
else
	echo "FAIL we never acked sanctum its MTU probes"
	fail=1
fi

if strip $DIR/sanctum.log | grep -q "our peer is unresponsive"; then
	echo "FAIL sanctum considered us unresponsive while idle"
	fail=1
else
	echo "ok   sanctum kept the tunnel up while we idled $IDLE seconds"
fi

if [ $fail -ne 0 ]; then
	echo "--- sanctum log"
	strip $DIR/sanctum.log | tail -30
	echo "--- vigil log"
	tail -30 $DIR/vigil.log
	exit 1
fi

echo "sanctum_interop: ok"
