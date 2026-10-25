# The chat vault

The chat vault keeps your message history on the phone. Everything in
it is encrypted. This document explains which keys protect it and what
happens to a message from the moment it arrives.

## The problem it solves

Messages can arrive at any time, also while the phone is locked. The
part of Vigil that receives them must be able to store them. But it
should not be able to read the history while the phone is locked.

The vault solves this by splitting the work in two. The network side can
only write new messages into a locked inbox. Only the app, while the
phone is unlocked, can open the inbox and read the history.

On iOS the network side is the packet tunnel extension. On Android it is
the VPN service.

## The keys

The vault uses three kinds of keys.

The inbox key is a P-256 key pair. Its private key can only be used
while the phone is unlocked. Its public key can be used at any time.

The outbox key is a 256-bit AES key. It can be used at any time after
the phone was unlocked once since boot. The network side needs it to
send messages that are still waiting.

Each conversation has its own 256-bit AES key. It is created at random
when the conversation starts. It can only be opened while the phone is
unlocked.

## On iOS

If the phone has a Secure Enclave, the inbox private key is created
inside it. It never leaves the Secure Enclave and can only be used while
the phone is unlocked. Without a Secure Enclave the private key is
stored in the keychain with the protection class "when unlocked, this
device only".

Each conversation key is sealed to the inbox public key and stored in
the keychain. Because opening it needs the inbox private key, a
conversation key can only be recovered while the phone is unlocked.

The outbox key is stored in the keychain with "after first unlock, this
device only".

All vault files live in the shared app group container. They use the
file protection class "complete until first user authentication" and are
excluded from backups.

## On Android

On Android 12 and later the inbox key pair is created in the Android
Keystore. If the phone has StrongBox, a separate security chip, the key
is created there. The key is marked so that it can only be used while
the phone is unlocked.

Conversation keys are encrypted with a separate AES key, the history
key. It also lives in the Keystore, in StrongBox when possible, and can
also only be used while the phone is unlocked.

The outbox key lives in the Keystore as well, but can be used while the
phone is locked.

Android versions before 12 cannot do key agreement inside the Keystore.
On those phones messages that arrive while the phone is locked are
encrypted with the outbox key instead. They are then protected by the
device key only, not by the unlock requirement. The app shows a warning
about this.

All vault files live in the app its no-backup directory.

## A message arrives

1. The core receives a message and hands it to the network side.
2. The network side creates a fresh P-256 key pair just for this
   message.
3. It combines that key with the inbox public key and derives an AES
   key with HKDF-SHA256.
4. It encrypts the message with AES-256-GCM and writes it as a new file
   in the inbox directory. The temporary key pair is thrown away.
5. It tells the core the message is stored. The core sends the
   acknowledgement to the sender.

The network side cannot open the inbox file it just wrote. Only the
inbox private key can.

## Opening the vault

When you open your chats with the phone unlocked, the app merges the
inbox:

1. It opens each inbox file with the inbox private key.
2. It adds the message to the log of its conversation.
3. It overwrites the inbox file with zeros and deletes it.

An inbox file that fails to decrypt is wiped and skipped.

## Conversation logs

Each conversation is one log file. New records are only added to the
end. Each record is encrypted with AES-256-GCM under the conversation
key.

The encryption is bound to the conversation id and the position of the
record in the log. This means:

- A record cannot be changed without being detected.
- Records cannot be moved within a log or swapped between logs.
- A record from another conversation cannot be inserted.

The number of records is stored together with the conversation key, in
its protected form. If the log has fewer records than expected, someone
removed messages from the end. The app then shows a warning. If a record
cannot be decrypted, the app shows the messages before it and warns that
the chat was damaged or tampered with.

Images and files are stored as separate files next to the log. Each is
encrypted with the conversation key. The encryption is bound to the
conversation and the file name, so files cannot be swapped between
conversations.

## The outbox and receipts

Messages you send are first added to your own conversation log. One-to-one
messages are also put in the outbox, so the network side can send them
when the peer comes online.

Delivery receipts work the other way. The network side writes them into
a receipts file. The app adds them to the conversation logs at the next
merge.

Both the outbox and the receipts file are encrypted with the outbox key.
They must be readable while the phone is locked, otherwise queued
messages could not be sent. Messages that wait in the outbox are
therefore protected less well than the history. They leave the outbox as
soon as they are delivered.

## Locking

While you read a chat, its conversation key is kept in memory. The app
drops all of these keys when the vault locks.

On iOS this happens when the app goes to the background or when iOS
announces that protected data is about to become unavailable. On Android
it happens when the screen turns off. On Android the key bytes are also
overwritten with zeros.

If the phone is locked when the app tries to read the vault, the
operation fails with a message asking you to unlock the device.

## Deleting a chat

Deleting a chat removes its conversation key, overwrites the log and
attachment files with zeros and deletes them. Waiting outbox messages
for that chat are removed too.

Flash storage may keep old copies of overwritten data. The real
protection is that the conversation key is gone. Without it the old data
cannot be decrypted.

## What is not in the vault

Peer names you set in the app and your own nickname are stored in the
normal app settings, not in the vault. Notification text is handled by
the operating system, see [gospel.md](gospel.md).

## Where this lives in the code

- `app/ios/Shared/ChatVault.swift` is the iOS vault.
- `app/android/app/src/main/kotlin/se/sanctorum/vigil/ChatVault.kt` is
  the Android vault.
