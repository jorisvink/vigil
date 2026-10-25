# Limitations

No app protects against everything. This document lists what Vigil does
not protect against, so you can decide what that means for you.

## The configuration is the identity

A configuration holds every key a device needs. Anyone who has it can act
as that device. This includes anyone who saw or photographed the QR code.

When you import a QR code from a picture, that picture stays in your
photo library. Vigil does not remove it.

The text of the QR code passes through the user interface as a string.
Such strings cannot be wiped from memory, they stay until the app is
restarted or the memory is reused. Vigil wipes every copy it holds as
raw bytes, but not these. See [erasure.md](erasure.md).

## What a cathedral sees

A cathedral cannot read, change or inject your traffic. It does see a lot
of metadata:

- the public IP addresses and ports of your devices
- which device ids in a flock are online, and when
- which devices take part in which liturgy or group chat
- which device wants to call or message which other device
- when traffic is relayed through it, how much and when

Shroud makes it harder for others on the network to see this. It does
not hide anything from the cathedral itself.

## The clock must be right

Key exchange messages carry a timestamp. If your phone its clock is off
by more than 10 seconds, tunnels will not come up.

## An unlocked phone

While the phone is unlocked and Vigil is running, the keys and the chat
history can be used. Someone with access to an unlocked phone, or with
malware running on it, can read your messages and use your tunnels.

## Messages that are still waiting

Messages in the outbox are encrypted with a key that works while the
phone is locked. They are less protected than the history until they are
delivered. See [vault.md](vault.md).

On Android versions before 12, messages that arrive while the phone is
locked are protected the same way.

## Notifications

With message previews turned on, the text of new messages is shown in
system notifications. The operating system may show these on the lock
screen and keep them for a while. Turn previews off if this matters to
you.

## Nicknames

The nickname a peer sends is not verified. A peer can choose any nickname,
including one that belongs to someone else. Name your peers yourself in
the app if you rely on names.

## When the tunnel is down

On iOS Vigil does not block traffic when the tunnel stops. Traffic then
leaves the phone outside the tunnel.

On Android you can turn on "always-on VPN" and "block connections
without VPN" in the system settings. The system then blocks traffic when
the tunnel is not running.

## Deleted data

Vigil overwrites files before it deletes them. Flash storage does not
guarantee that this removes every copy. Deleted chats stay unreadable
because their keys are removed, not because the files are gone.
