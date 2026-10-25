# Storing configurations

A configuration holds every key a device needs. This document describes
how Vigil stores configurations on the phone and how it handles the keys
in memory.

## Configurations and summaries

Vigil keeps two things for each imported configuration.

The configuration itself contains the keys. It is always stored
encrypted.

The summary is a small JSON document made by the core. It holds the name,
mode, addresses, routes and DNS servers. It never contains keys. The user
interface and the VPN setup code only work with the summary. This means
Vigil can list your tunnels without decrypting anything.

## On iOS

Configurations are stored in the iOS keychain as generic password items.
The items are placed in an access group that the app and its packet
tunnel extension share. No other app can read them.

The items use the protection class "after first unlock, this device
only". This has two effects:

- The tunnel can start while the phone is locked, as long as it has been
  unlocked once since it booted.
- The items are not included in backups and are not moved to a new
  phone.

The app keeps a list of tunnels in its settings. Each entry has the
summary and a keychain reference. The reference only points to the item,
it does not contain the configuration.

When you connect, the app hands iOS the keychain reference as part of the
VPN profile. The packet tunnel extension uses that reference to load the
configuration from the keychain itself. The configuration does not pass
through the app at that point.

## On Android

Configurations are stored as files in the app its no-backup directory.
Each file is encrypted with AES-256-GCM. The key is created inside the
Android Keystore and cannot be read out of it. The app can only ask the
Keystore to encrypt or decrypt.

Next to each encrypted file the summary is kept as plain JSON. It has no
keys in it.

Android backups are turned off for the whole app. The data extraction
rules exclude all app data from cloud backup and from device transfer.

When a configuration is removed, its file is first overwritten with
zeros and then deleted.

## Editing a configuration

A few options can be changed in the app: the name, DNS servers, routes,
p2p and tier6. Keys cannot be changed.

An edit works on the decrypted configuration in the core. The core
replaces only the allowed options and then parses the result as if it
were a new import. If the result is not valid, the edit is refused and
the old configuration stays. A valid result is encrypted again and
replaces the old one.

## Keys in memory

Vigil tries to keep keys in memory for as short a time as possible.

The core overwrites every buffer that held a configuration with zeros
once it is done with it. This includes the parse buffers, the parsed
configuration when it is freed, and configurations waiting to be picked
up by the running tunnel.

The platform code does the same with its own copies. On Android the byte
arrays are filled with zeros after they were handed to the core. The JNI
layer copies the bytes into C memory and wipes that copy too. On iOS the
byte arrays loaded from the keychain are overwritten with zeros once the
core has them.

While a tunnel runs, the keys live inside libkyrka contexts. Only one
thread may use these contexts at a time. The core enforces this with a
lock.

## Emergency erase

If the core runs into an error it cannot recover from, it calls the
libkyrka emergency erase before it stops the process. This wipes all key
material held by all libkyrka contexts.

If another thread is using libkyrka at that moment, the erase waits up to
two seconds for it to let go. The other thread is then blocked from ever
using libkyrka again. This makes sure no code keeps using a context that
was just wiped.

## Other files

When a cathedral tells the device about other cathedrals in its
federation, Vigil saves that list in its data directory. This is called a
remembrance. It holds cathedral addresses only, no keys. The file is
created with permissions that only allow the app to read it.

## Where this lives in the code

- `src/remembrance.c` stores the cathedral list.
- `src/config.c` creates summaries and applies edits.
- `src/vigil.c` holds the libkyrka lock and the emergency erase.
- `app/ios/Shared/TunnelKeychain.swift` stores configurations on iOS.
- `app/android/app/src/main/kotlin/se/sanctorum/vigil/TunnelStore.kt`
  stores configurations on Android.
