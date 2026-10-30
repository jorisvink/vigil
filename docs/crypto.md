# Cryptographic description

Vigil has two layers of cryptography.

The network layer is libkyrka, the sanctum protocol library. All
traffic that leaves the phone is protected by it: tunnels, calls, chats
and the messages to cathedrals. Vigil uses it as is, with the same
algorithms, keys and KDF labels as sanctum. The shared secret, KEK,
cathedral secret, COSK, ambries, session keys and shroud are described
in the sanctum crypto documentation:
https://github.com/jorisvink/sanctum/blob/master/docs/crypto.md

The storage layer is the platform code, Kotlin on Android and Swift on
iOS. It protects configurations and chat history on the phone with the
keystores of the operating system. This document describes the storage
layer and the keys Vigil creates itself.

## Algorithms

| Use | Algorithm | Implementation |
| --- | --------- | -------------- |
| Storage encryption | AES256-GCM | Android Keystore, Conscrypt, CryptoKit |
| Chat inbox key agreement | ECDH over P-256 | Android Keystore, Secure Enclave, CryptoKit |
| Chat inbox KDF | HKDF-SHA256 | hand written HMAC-SHA256 (Android), CryptoKit (iOS) |

## Labels

The chat vault uses ASCII strings prefixed with "VIGIL.CHAT.". They are
used as AAD and as HKDF info, see "The chat vault" below.

## Keys

Vigil does not create network keys. They arrive in a configuration,
usually from a QR code, and are only handed to libkyrka. Vigil does
create its own keys for local storage and for the link between the iOS
app and its extension.

### TL;DR

| Key | Description | Type | Where it lives |
| --- | ----------- | ---- | -------------- |
| LS | Link secret between the iOS app and its extension. | Red | iOS keychain |
| TK | Tunnel store key, encrypts configurations on Android. | Red | Android Keystore |
| HK | History key, wraps conversation keys on Android. | Red | Android Keystore |
| OK | Outbox key, protects the chat outbox and receipts. | Red | Android Keystore, iOS keychain |
| IK | Inbox key pair, P-256, messages are sealed to it. | Red | Android Keystore, Secure Enclave |
| CK | Conversation key, one per chat. | Red | wrapped by HK or sealed to IK |

A red key must never be seen by anyone else
(https://csrc.nist.gov/glossary/term/red_key).

## The link secret (LS)

On iOS the app and the packet tunnel extension exchange chat messages,
call signaling and status through iOS. Vigil sends these messages
through a libkyrka tunnel between the two, so they are encrypted and
authenticated like any other tunnel traffic:

```
ls = SecRandomCopyBytes(256-bit)
keychain item: service "se.sanctorum.vigil.link"
               access group "group.se.sanctorum.vigil"
               kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
```

Both sides load the LS as the shared secret of that tunnel. The LS is
never put in a message. The app creates a new one when it connects
after the extension stopped cleanly.

Android does not need a link, the app and the VPN service share a
process.

## Configuration storage

On Android every configuration is encrypted with the tunnel store key:

```
tk = Keystore AES-256, alias "se.sanctorum.vigil.tunnels", GCM, no padding
file = nonce || AES256-GCM(tk, nonce, aad=none, configuration) || tag
```

The 96-bit nonce is chosen by the Keystore. The key cannot be read out of the
Keystore. The summary is kept next to it as plain JSON without keys.

On iOS a configuration is a keychain item and gets no extra encryption
from Vigil:

```
keychain item: service "se.sanctorum.vigil.tunnel"
               access group "group.se.sanctorum.vigil"
               kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
```

The extension loads it by persistent reference. See
[storage.md](storage.md).

## The chat vault

The chat vault keeps message history on the phone. The network side
can write new messages, only the app can read the history. See
[vault.md](vault.md) for the reasoning, this section has the
constructions.

Notation, used for both platforms:

```
GCM(k, aad, m) = nonce || AES256-GCM(k, nonce, aad, m) || tag
                 nonce is 96-bit, selected uniformly at random

label(chat)    = the peer id as one byte for a one-to-one chat,
                 the ASCII chat id ("g0001") for a group chat
seq            = position of a record in its log, 64-bit big endian
```

### Keys on Android

```
hk = Keystore AES-256, alias "se.sanctorum.vigil.chat.history",
     setUnlockedDeviceRequired(true), StrongBox if available
ok = Keystore AES-256, alias "se.sanctorum.vigil.chat.outbox",
     StrongBox if available
ik = Keystore EC P-256, alias "se.sanctorum.vigil.chat.inbox",
     PURPOSE_AGREE_KEY, setUnlockedDeviceRequired(true),
     StrongBox if available, Android 12 and later only
```

The nonce for every operation with a Keystore key is chosen by the
Keystore.

### Keys on iOS

```
ik  = Secure Enclave P-256 key agreement key,
      kSecAttrAccessibleWhenUnlockedThisDeviceOnly, .privateKeyUsage
      without a Secure Enclave: a CryptoKit P-256 key
ok  = 256-bit random from CryptoKit

keychain items, service "se.sanctorum.vigil.chat",
access group "group.se.sanctorum.vigil":

    "inbox.private"  ik, as Secure Enclave handle or raw key
                     WhenUnlockedThisDeviceOnly
    "inbox.public"   ik public key, X9.63
                     AfterFirstUnlockThisDeviceOnly
    "outbox"         ok
                     AfterFirstUnlockThisDeviceOnly
    "key.<chat>"     the sealed conversation key, see below
                     WhenUnlockedThisDeviceOnly
```

The Secure Enclave handle can only be used by the Secure Enclave of the
phone that created it.

### Sealing to the inbox key

Incoming messages are sealed to the inbox public key, so the network
side never needs the private key:

```
seal(m, info):
    e = P-256-KEYGEN()                    fresh for every message
    ss = ECDH(e.private, ik.public)

    E = e.public, S = ik.public
        Android: DER X.509 SubjectPublicKeyInfo, 91 bytes
        iOS: X9.63, 65 bytes

    k = HKDF-SHA256(ikm=ss, salt=E || S, info=info), 256-bit
    return 0x01 || len(E) (16-bit big endian) || E || GCM(k, info, m)

open(sealed, info):
    ss = ECDH(ik.private, E)
    k = HKDF-SHA256(ikm=ss, salt=E || S, info=info), 256-bit
    return GCM-OPEN(k, info, ...)
```

On Android HKDF is written out with HMAC-SHA256 and one output block:

```
prk = HMAC-SHA256(salt, ss)
k   = HMAC-SHA256(prk, info || 0x01)
```

On iOS it is CryptoKit its `hkdfDerivedSymmetricKey()`.

The ECDH with the private key runs inside the Keystore or the Secure
Enclave. Every sealed message has its own key, `e` and `k` are thrown
away after use.

On Android before version 12 there is no Keystore key agreement. There
an incoming message is stored as `0x02 || GCM(ok, "VIGIL.CHAT.INBOX", m)`
and is only protected by the outbox key.

### Inbox, outbox and receipts

```
inbox/<time>-<random>.box = seal(message, "VIGIL.CHAT.INBOX")
outbox.box                = GCM(ok, "VIGIL.CHAT.OUTBOX", json list)
receipts.box              = GCM(ok, "VIGIL.CHAT.RECEIPTS", json list)
```

### Conversation keys (CK)

Every chat gets its own key when it starts:

```
ck    = 256-bit random       SecureRandom on Android, CryptoKit on iOS
plain = ck || count || read || flags, each counter 64-bit big endian

Android: <chat>.key       = GCM(hk, "VIGIL.CHAT.KEY" || label(chat), plain)
iOS:     key.<chat> item  = seal(plain, "VIGIL.CHAT.KEY" || label(chat))
```

`count` is the number of records in the log, `read` how many were seen
and `flags` marks a log that was cut short.

### Logs and attachments

```
<chat>.log, per record:
    len(sealed) (32-bit big endian) ||
    sealed = GCM(ck, "VIGIL.CHAT.V1" || label(chat) || seq, record)

<chat>-<ref>.dat:
    GCM(ck, "VIGIL.CHAT.DATA" || label(chat) || file name, data)
```

The AAD binds a record to its conversation and its position, and an
attachment to its conversation and its file name. A record cannot be
changed, moved or copied into another chat without failing to open.
Records that were cut from the end are found by comparing the log with
`count`.

### Nonces

All vault nonces are 96-bit random, no counters. A log can be truncated
by someone with access to the files, after which `seq` values are used
again under the same key. A counter nonce would then repeat, a random
nonce does not.

The conversation key and the history key take the most encryptions, a
few per message. That stays far below the 2^32 encryptions NIST allows
per key with random 96-bit nonces.

## Randomness

| What | Source |
| ---- | ------ |
| Link secret | SecRandomCopyBytes |
| Conversation keys and vault nonces on Android | SecureRandom |
| Conversation keys, outbox key and vault nonces on iOS | CryptoKit |
| Keystore nonces and keys | Android Keystore |
| Inbox key pairs | Android Keystore, Secure Enclave or CryptoKit |

## Where this lives in the code

- `src/link.c` and `app/ios/Shared/TunnelLink.swift` run the link.
- `app/android/.../TunnelStore.kt` and
  `app/ios/Shared/TunnelKeychain.swift` store configurations.
- `app/android/.../ChatVault.kt` and `app/ios/Shared/ChatVault.swift`
  are the chat vault.
