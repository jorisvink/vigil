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
- domain `0x0c` for group chats

Because the domain is part of the derivation, a call tunnel and a chat
tunnel between the same two devices never share keys.

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

## Where this lives in the code

- `src/config.c` parses and checks configurations.
- `src/peer.c` loads the keys into libkyrka for tunnels.
- `src/voice.c`, `src/call.c` and `src/chat.c` do the same for calls and
  chats.
- The sanctum documentation in `docs/crypto.md` of the sanctum project
  has the full key derivation details.
