# Calls and messages

Vigil can make voice and video calls and send text messages, images and
files. All of this runs over sanctum tunnels. The protocol is called gospel.
It is compatible with the confessions and gospel projects.

## The gospel configuration

Calls and messages use their own configuration, with mode `gospel`. It
has the same keys as a cathedral configuration: a KEK, a cathedral
secret, a COSK, a flock and a device id. There can only be one gospel
configuration on a device.

A gospel configuration can run on its own. In that case no VPN interface
is created at all. It can also run next to a normal tunnel. It then
shares the core with that tunnel but uses its own sockets, contexts and
keys.

Each device in the flock has an id from 1 to 254. That id is how other
devices address you. It is also how Vigil knows who sent a message.

## Separate keys for each purpose

Calls, one-to-one chats and group chats each use their own flock domain,
see [keys.md](keys.md). Every call and every chat tunnel also has its own
tunnel id, made from the ids of the two devices. The cathedral hands out
a separate ambry for each of those.

Together this means that a call between two devices and a chat between
the same two devices never share a key.

## Signaling

Devices do not keep a tunnel open to every other device. Instead they
tell the cathedral which devices they want to reach. This is done
through a liturgy, the same mechanism that is used for meshes.

For calls, the device sends the cathedral a short list of the ids it
wants to call. The cathedral passes this on to the callee. The callee
then knows that someone wants to call it, and who.

Chats work the same way. A device that has messages waiting for a peer
says so in the chat signaling liturgy. The peer sees this and both sides
set up a chat tunnel.

These signaling messages go to the cathedral only. They are encrypted
with the cathedral secret and signed with the COSK. They carry no keys
and no message content.

## Calls

Once both sides know about the call, each creates a call tunnel to the
other. The call tunnel does a normal key exchange. Audio and video only
flow once that exchange has finished.

When a device is told that someone is calling, it waits for that caller
to show up on the call tunnel. If the caller does not appear within 10
seconds, the call is dropped. A call cannot ring through without a
working tunnel to the caller.

Audio is encoded with Opus. Every voice frame on the wire has the same
size, whether it carries speech, silence or a hang up. Video frames are
split into pieces that fit in a packet. All of it is encrypted with the
session keys of the call tunnel.

A call tries to go directly between the two devices, just like a
tunnel. If that does not work, it is relayed through the cathedral. The
cathedral cannot listen in.

On Android, calls run inside the VPN service. On iOS, calls run inside
the app, because the packet tunnel extension cannot use the microphone
or camera. The app loads the gospel configuration from the keychain for
the length of the call and wipes it afterwards. Signaling still runs in
the extension.

## One-to-one chats

A one-to-one chat uses a tunnel between two devices in the chat domain.
The tunnel is created when there is something to send and closed again
after a minute without activity.

Every chat frame on the wire has the same size. A text message, an
acknowledgement and a heartbeat all look the same to someone watching
the network. Messages can be up to 512 bytes. Images and files are split
into chunks and can be up to 4 MB.

Each device also sends a nickname in its heartbeats. The nickname is
chosen by the sender and is not checked by anyone. The device id is what
identifies the sender. You can give peers your own names in the app.

## Group chats

A group chat is a liturgy in the group chat domain. Every member tells
the cathedral it is in the group id, and the cathedral returns the list
of members that are online in that group.

There is no group key. Each device sets up a separate tunnel to every
other member, with its own keys. A group message is sent over each of
those tunnels in turn. The sender sees how many members received it.

## Delivery

A message counts as delivered only after the receiver has stored it.

When the core receives a message, it hands it to the platform code with
a token. The platform code writes the message into the chat vault, see
[vault.md](vault.md). Only then does it tell the core that the message
with that token was stored. The core then sends the acknowledgement back
to the sender.

If the platform code does not confirm within 10 seconds, the core hands
the message over again. Duplicates are recognised and dropped. A message
is never acknowledged if it was only received and then lost.

One-to-one messages wait in an outbox until they are delivered. They are
sent again when the peer comes back, even after the app was restarted.
Group messages are only sent to members who are online at that moment.

## Notifications

New messages and incoming calls can show a system notification. By
default the notification shows the message text. You can turn previews
off in the app. The notification then only says that there is a new
message.

Notification text is handled by the operating system. It is not part of
the chat vault and can be shown on the lock screen.

## Where this lives in the code

- `src/call.c` runs call signaling.
- `app/ios/Runner/VigilCall.swift` runs calls on iOS.
- `src/voice.c` and `src/video.c` run call tunnels, audio and video.
- `app/android/.../Chats.kt` and `Calls.kt` connect these to Android.
- `src/chat.c` runs chat signaling, chat tunnels, groups and delivery.
- `app/ios/PacketTunnel/PacketTunnelProvider.swift` handles chats on iOS.
