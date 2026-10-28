# Tunnels

This document describes the tunnel modes Vigil supports and how a packet
travels from an app on your phone to the other side.

## The path of a packet

When you connect, the platform code creates a VPN interface. On Android
this is done by a VPN service. On iOS it is done by the packet tunnel
extension. The interface is built from the summary, so the address,
routes and DNS servers come from your configuration.

The core then reads IP packets from that interface. For each packet it
picks the peer that should get it and hands the packet to libkyrka.
libkyrka encrypts it with the current session key and gives it back. The
core sends the result over UDP.

Incoming packets take the opposite path. A UDP packet is handed to
libkyrka, which checks and decrypts it. Only packets that pass this check
are written to the VPN interface. Anything that fails authentication is
dropped.

The UDP sockets the core uses are kept out of the tunnel. On Android each
socket is marked as protected through the VPN service. On iOS the
extension its own traffic is not routed into its own tunnel. Without this
the encrypted packets would loop back into the tunnel.

Next to normal packets, peers exchange small control messages inside the
encrypted tunnel. These are heartbeats and MTU probes. They travel under
the same session keys as your traffic.

## Tunnel mode

This is a classic client to server VPN. The configuration has the
address of one peer and a shared secret. There is no cathedral.

The device talks to the peer directly. Session keys are created as
described in [keys.md](keys.md).

## Cathedral mode

In cathedral mode the configuration has no shared secret. It has a KEK,
a cathedral secret, a COSK, a flock and a tunnel id instead.

The device tells the cathedral it is online. Those messages are encrypted
with the cathedral secret and signed with the COSK. The cathedral then
does two things for the device:

- It hands over the ambry for this tunnel. The device unwraps it with its
  KEK and gets the shared secret.
- It tells the device where its peer can be reached.

From that point the two devices run the normal key exchange with each
other. The cathedral is not part of that exchange and never learns the
session keys.

## Peer to peer and relaying

By default Vigil tries to reach its peer directly, even when both sides
are behind NAT. The cathedral tells each side the public address of the
other. Both sides then send heartbeats to each other more often for a
while to open up a path through their NAT.

The cathedral only hands out these addresses once a tunnel has been up
for about 15 seconds. Until then, traffic is relayed.

A direct path has to prove that it works. A tunnel sends a small probe
over it every second, and the path works once the peer answers one of
those probes over that same path. A call or chat counts the path as
working once something from the peer arrives over it. Tunnels and chats
give a new path 30 seconds, calls give it 10 seconds.

If the path does not work in time, Vigil goes back to sending through
the cathedral. The path is not tried again for 30 seconds. Each time it
fails again that wait doubles, up to 30 minutes. A path that works or a
network change starts the wait over.

When the peer reaches us over a direct path while we still relay, we
move to that path as well. In calls and chats it also works the other
way around. When the peer reaches us through the cathedral while we are
on a direct path, the peer cannot use that path, and we go back to the
cathedral too.

If both devices share the same public address, calls and chats always
relay through the cathedral. Many routers cannot send traffic back into
their own network. Tunnels do the same, unless tier6 is on, see below.

When the cathedral relays, it only forwards encrypted packets. It cannot
read or change them. Direct paths can be turned off in the configuration
with the `p2p` option.

## Liturgy mode

A liturgy is a full mesh of devices in the same flock and group. Each
device tells the cathedral it takes part. The cathedral replies with the
list of devices that are present. Vigil creates a separate tunnel with its
own libkyrka context and keys for every device in that list.

The connection to the cathedral used for the liturgy only holds the
cathedral secret and the COSK. The KEK is only loaded into the tunnels
to the other devices.

In a liturgy each device has a fixed address based on its id. A packet
coming out of a peer its tunnel is only accepted if its source address
matches that peer. One device in the mesh cannot pretend to be another.

A device can choose not to be discoverable. It then does not show up in
the list for others.

## Tier6

With tier6 turned on, the tunnels carry ethernet frames instead of IP
packets. This allows talking to the tier6 daemon, which builds a layer 2
network. The key handling is the same as in the other modes.

When two tier6 peers share the same public address, they look for each
other on their local networks. Each side sends the other its local IPv4
addresses inside the tunnel. The other side then sends a discovery probe
to each of those addresses. It only moves the tunnel to an address that
answered one of its own probes. These probes travel under the session
keys like everything else.

## Commixtion

With `cathedral_commixtion` turned on, relayed traffic does not go from
the cathedral straight to the peer. It travels through two cathedrals of
the federation first. This makes it harder to tie the two ends of a
tunnel together by watching a single cathedral.

Commixtion requires shroud and turns off direct paths, so all traffic is
relayed. Calls and chats use it as well when the gospel configuration
has it turned on.

## Gospel mode

A gospel configuration is only used for calls and messages. It can run
on its own, or next to a normal tunnel. See [gospel.md](gospel.md).

When it runs on its own, Vigil still creates a VPN interface so the
system keeps it running. That interface has a single local address and
no routes, so no traffic goes through it.

When it runs next to a tunnel that sends all traffic through the VPN,
the route to the gospel cathedral is left out of the tunnel. Calls and
chats then do not travel inside the other tunnel.

## Dead peers

If nothing has been heard from a peer for two minutes, the core asks
libkyrka to throw away the session keys for that peer. A fresh key
exchange starts once the peer is back. Keys are not kept around for a
peer that may be gone.

## Routes, DNS and IPv6

Only the routes in the configuration go through the tunnel. A route of
`0.0.0.0/0` sends all IPv4 traffic through it.

When all IPv4 traffic goes through the tunnel, Vigil also takes all IPv6
traffic. It routes that to an address that goes nowhere. This keeps IPv6
traffic from leaving the phone outside the tunnel. You can turn this off
with the `block_ipv6` option.

The DNS servers in the configuration are given to the system. On iOS
they are used for all lookups while the tunnel is up.

On Android the system setting "block connections without VPN" works with
Vigil. When it is on, Vigil logs a note because only the routes in the
tunnel will work.

## Network changes

Phones move between networks all the time. When the network changes,
Vigil opens new sockets and tells the cathedrals about itself right away.
The session keys do not change because of this. Only the path does.

## Where this lives in the code

- `src/ether.c` handles tier6.
- `src/grace.c` handles heartbeats and MTU probes.
- `src/liturgy.c` runs the liturgy with the cathedral.
- `src/vigil.c` runs the event loop and reads the VPN interface.
- `src/peer.c` runs the tunnels, direct paths and dead peer checks.
- `app/ios/PacketTunnel/PacketTunnelProvider.swift` builds the iOS one.
- `app/android/.../VigilVpnService.kt` builds the Android VPN interface.
