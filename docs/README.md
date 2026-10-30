# Vigil security documentation

Vigil is a VPN and communication app for iOS and Android. It speaks the
sanctum protocol through libkyrka. It can run normal VPN tunnels, mesh
networks between devices, and encrypted calls and text messages.

These documents explain how Vigil protects your data. They follow the
keys from the moment you scan a QR code until they are erased again.
They also explain how chat messages are stored on the phone.

## The parts of the app

Vigil is built from four layers.

The core is written in C and lives in `src/`. It parses configurations,
runs tunnels, calls and chats, and is the only code that touches tunnel
keys while a tunnel runs. It is the same on both platforms.

libkyrka is the sanctum protocol library. The core hands it the keys and
the packets. libkyrka does the key exchanges and all encryption of
network traffic (https://github.com/jorisvink/libkyrka).

The platform code is written in Kotlin on Android and Swift on iOS. It
stores configurations, creates the system VPN interface, handles the
camera and microphone, and owns the chat vault.

The user interface is written in Flutter (Dart). It never sees tunnel
keys after import. It only works with summaries that have the keys
removed.

On Android everything that touches the network runs inside a VPN
service. On iOS the network side runs in a packet tunnel extension, which
is a separate process from the app. Calls on iOS run in the app itself,
because only the app can use the microphone and camera.

## Reading order

1. [keys.md](keys.md) describes every key in a configuration and how
   libkyrka turns them into session keys.
2. [storage.md](storage.md) describes where configurations are kept on
   the phone and how keys are handled in memory.
3. [erasure.md](erasure.md) describes how keys are erased from memory
   and which copies remain.
4. [tunnels.md](tunnels.md) describes the tunnel modes and how packets
   flow through them.
5. [gospel.md](gospel.md) describes how calls and messages work.
6. [vault.md](vault.md) describes the chat vault, which keeps message
   history encrypted on the phone.
7. [crypto.md](crypto.md) describes the keys Vigil creates itself and
   how configurations and chats are encrypted on the phone.
8. [limitations.md](limitations.md) lists what Vigil does not protect
   against.

## Who you trust

Whoever created your configuration holds the same keys you do. Vigil
never creates tunnel keys itself. You trust that person or system to
keep them safe.

A cathedral is a server that helps devices find each other and can relay
their traffic. It cannot read, change or inject encrypted traffic. It
does see who is online and who is talking to whom. See
[limitations.md](limitations.md) for details.
