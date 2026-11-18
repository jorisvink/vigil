# Keys

This document describes the key material Vigil works with. It covers
where the keys come from, what each one is for, and how they become the
keys that actually encrypt your traffic.

## Where keys come from

All tunnel keys arrive in a configuration. A configuration is a short
text file, usually delivered as a QR code. It is created outside of
Vigil, for example by a Reliquary account or by the `tools/vigil-config`
script.

Vigil does not generate tunnel keys. It only imports them. Anyone who has
seen the QR code has the keys, so treat it like a password.

A configuration holds some or all of these keys:

- `secret`, the shared secret
- `kek`, the key-encryption-key
- `cathedral_secret`, the cathedral secret
- `cathedral_cosk`, the cathedral offer signing key

A configuration made by Covenant also holds:

- `name_pubkey`, the public key the flock signs device names with
- `name_signature`, the signature over the `instance` of this device

These two are not secret. See [gospel.md](gospel.md) for how they are
used.

Each key is written as hex. The core checks the length of every key when
it parses the file. A configuration with a missing or malformed key is
rejected.

## The shared secret

The shared secret is a 256-bit key that two peers have in common. It is
only used in direct tunnel mode, where Vigil talks to one known server.

The shared secret is never used to encrypt traffic directly. libkyrka
derives separate keys from it for each purpose. One key protects the key
exchange messages, others feed into the session keys and the shroud.

A configuration that uses a cathedral may not contain a shared secret.
The core refuses such a configuration. With a cathedral, the shared
secret is delivered as an ambry instead, see below.

## The key-encryption-key

The KEK is a 256-bit key that is unique to your device. It is used to
unwrap ambries.

An ambry is a shared secret that has been encrypted with your KEK. The
cathedral holds the ambries for your flock and hands the right one to your
device when needed. The cathedral cannot open an ambry because it does
not have your KEK. Only your device can.

This lets an operator rotate shared secrets for a whole flock by
publishing new ambries. Devices do not need a new configuration for that.

## The cathedral secret

The cathedral secret is a 256-bit key shared between your device and the
cathedral. It is tied to a 32-bit identity, the `cathedral_id`.

It protects the messages your device sends to the cathedral and the
replies it gets back. The cathedral uses it to prove to your device that
it is the real cathedral.

## The cathedral offer signing key

The COSK is an ed25519 private key. Your device signs everything it sends
to the cathedral with it. The cathedral holds the matching public key for
your identity.

When cathedrals federate, they keep these signatures intact. A cathedral
in the federation can therefore not pretend to be your device.

## Session keys

The keys above are long term keys. The keys that actually encrypt your
packets are session keys. libkyrka creates them with a key exchange
whenever a tunnel comes up and replaces them regularly.

The exchange works like this:

1. Both sides create an offer. It holds a fresh x25519 public key and a
   fresh ML-KEM-1024 public key.
2. The offer is encrypted with a key derived from the shared secret and
   sent to the peer.
3. The peer answers with an ML-KEM ciphertext and its own x25519 key.
4. Both sides now have an x25519 result and an ML-KEM result. They mix
   these with a key derived from the shared secret using KMAC256.
5. This gives one key for sending and a different key for receiving.

The result is a hybrid exchange. An attacker would have to break x25519,
ML-KEM-1024 and the shared secret to learn a session key.

Traffic is encrypted with AES-256-GCM. Each direction uses its own key
and a 64-bit packet counter for the nonce. New keys are negotiated when a
key has been used for too many packets or for too long.

Each offer carries a timestamp. A peer drops offers that are more than
10 seconds old or 10 seconds in the future. The clock on your phone must
therefore be roughly correct.

## Flocks and domains

A flock is a network on a cathedral. Its id is 64 bits. The upper 56 bits
name the flock and the lowest 8 bits are the domain.

The flock ids go into every key derivation. Two devices in different
flocks get different keys, even if everything else is the same.

Vigil uses domains to separate its different uses of the same keys:

- domain `0x00` for normal tunnels and liturgies
- domain `0x0a` for calls
- domain `0x0b` for one-to-one chats
- domain `0x0c` for the chat signaling liturgy
- domain `0x0d` for the safety code exchange

Because the domain is part of the derivation, a call tunnel and a chat
tunnel between the same two devices never share keys.

## Safety codes

A safety code is an extra 256-bit secret that two peers share. The
secret is called the additional input. Vigil only uses the additional
input for one-to-one chats and calls between those two peers.

### How the input is used

libkyrka derives the base keys for a peer with KMAC256, see above. When
an additional input is loaded, libkyrka appends the length of the input
and the input to the data of that KMAC256:

	x = len(flock_a) || flock_a || len(flock_b) || flock_b
	x = x || len(input) || input

This applies to the peer offer key, the traffic keys and the peer shroud
key. The cathedral offer key, the cathedral shroud key and the KEK unwrap
are not changed. An attacker now also needs the additional input to learn
a session key.

Without a safety code the core loads no additional input, and the keys
are the same as without the feature. If only one of the two peers has an
input, or the inputs differ, the chat and call tunnels between them do
not come up.

The support lives in `dist-build/patches/libkyrka-kdf-input.patch`, which
adds `kyrka_additional_kdf_input()` to libkyrka.

### Making a safety code

Both peers open the safety code page at the same time, in person or over
a call. Each phone makes a fresh 64-bit half and shows the half as 5
words from the same 7776 word list Covenant uses. The words are a number
in base 7776, most significant word first. Each user reads their words
to the other user and types the words of the other user.

The two halves give the secret for a tunnel between both phones:

	x = len(flock) || flock || len(low) || low || len(high) || high
	secret = KMAC256(half_low || half_high, "VIGIL.SAFETY.KDF.SECRET", x)

Here low and high are the KEK ids of both peers, half_low is the half of
the peer with the lowest KEK id and flock is the cathedral flock. The
secret is the shared secret of a cathedral tunnel in domain `0x0d`. This
tunnel uses no ambry and no KEK, so only the words protect the tunnel.

The secret is used as the authentication mechanism for this temporary tunnel.
If the words do not match, the key exchange never succeeds.

Over that tunnel each phone sends a fresh 128-bit share once a second
until the other phone acks the share. Each phone answers every share
from the other phone with a burst of 5 acks. A phone is done once the
phone has:

- the share of the other phone
- an ack for its own share
- sent its acks for the share of the other phone

The additional input is then:

	input = share_low || share_high

Both phones wipe the shares and close the tunnel. The words and halves
are never stored. If no tunnel comes up within two minutes the attempt
fails and both users start over with new words.

If all 5 acks from one phone are lost, that phone has the input and the
other phone does not. The chats between both peers then fail until the
users make a new safety code.

### Storing the input

The platform code stores the additional input per peer in
`chats/safety/<peer>.box`. The file holds a random key wrapped with the
outbox key, followed by the input sealed with AES-256-GCM under that
random key. The outbox key is readable while the phone is locked, so the
Gospel service loads every input when the service starts.

Each peer has an on and off switch. When the switch is off, the core gets
no additional input for that peer but the file stays. Renewing the safety
code runs a new exchange and replaces the file. A desktop companion has no
safety codes of its own, the chat tunnels of the desktop run on the phone.

### Comparing safety codes

Once a safety code is set, both phones show a code made from the
additional input:

	code = KMAC256(zero key, "VIGIL.SAFETY.CODE", input)

The key is 32 zero bytes. The phone shows the first 64 bits of the output
as 16 hex digits in groups of 4. Both users compare the codes to check
that both phones hold the same additional input. The code reveals nothing
useful about the input.

## Shroud

Shroud is optional. When it is turned on, each packet header is masked
with a value derived from a shroud key. Traffic to a cathedral uses a
key derived from the cathedral secret. Traffic to a peer uses a key
derived from the shared secret.

Shroud makes it harder for someone on the network to tell which devices
belong together. It is traffic analysis protection, not encryption. The
packets are already encrypted with the session keys. All devices and the
cathedral must agree on shroud for it to work.

## Life of a key inside Vigil

This is the path the keys take through the app:

1. The QR code is scanned and the text is passed to the platform code.
2. The core parses the text and checks it. It returns a summary without
   any key material.
3. The platform code encrypts the configuration and stores it, see
   [storage.md](storage.md).
4. When you connect, the platform code decrypts the configuration and
   passes the bytes to the core. It then overwrites its own copy with
   zeros.
5. The core parses the configuration again and loads the keys into one
   libkyrka context per tunnel.
6. When the tunnel stops, the contexts are freed and the parsed
   configuration is overwritten with zeros.

The keys are never written to disk in the clear by Vigil.
