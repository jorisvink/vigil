# Erasing keys from memory

Vigil overwrites the memory that held a configuration once it no longer
needs it. This document describes how far that goes. Some copies of a
configuration are made by code Vigil does not control, and those are
not erased.

## What a wipe can and cannot do

A wipe overwrites one buffer, at the address it has at that moment. It
does nothing for copies that were made before. Those copies come from
three places:

- Garbage collectors. Java (ART) and Dart move small objects around in
  memory. The old location is not cleared, it is reused later.
- Strings. Java, Kotlin, Dart and Swift strings cannot be changed, so
  they cannot be overwritten. They stay in memory until the garbage
  collector reuses the space.
- Libraries and the system. The Flutter engine, the Android Keystore
  and the iOS keychain make their own copies of the data we pass them.

Freed memory is not cleared by the system either. A copy stays readable
until something else is written over it.

## How each layer wipes

In C the core uses `nyfe_mem_zero()`. It writes through a volatile
pointer, so the compiler cannot remove the writes. This is the only
layer with that guarantee.

In Kotlin `ByteArray.fill(0)` writes zeros into the same array, it does
not allocate a new one. ART does not remove these writes because the
arrays are passed on to other code, but no specification promises this.

In Swift `TunnelKeychain.wipe()` uses `memset_s()`, which may not be
removed by the compiler. A plain loop that sets each byte to zero could
be removed.

In Dart `fillRange()` writes zeros into the same list. The Dart compiler
does not remove these writes today, but no specification promises this
either.

## What we measured

We tested this on the Android emulator (Android 15, debug build). We
imported a configuration with a known random secret, then read the
memory of the app process at each step. We searched for the secret as
hex text in 8-bit and 16-bit characters, and for the raw 32 key bytes.

Importing by pasting text left copies of the configuration text behind:

- About 7 copies in the Dart heap. These are strings: the text field,
  the configuration passed to the review page and trimmed copies of it.
- About 3 copies in the Java heap. Flutter mirrors the text field into
  Java strings for the keyboard.
- 1 to 3 copies in native memory. These are the Flutter engine's text
  input state and its message buffers.

These copies were still there after the tunnel stopped and after a Java
garbage collection. They stay until the app is restarted or the memory
is reused. Importing by QR code or handoff goes through the same Dart
strings, without the text field.

Using a stored configuration left no text copies once the tunnel was
stopped:

- While the tunnel ran, the configuration text was in native memory
  once (the copy the core runs from) and the raw secret once (the
  parsed configuration). Both were gone after the tunnel stopped.
- Decrypting a stored configuration leaves 2 copies of the text in the
  Java heap. They are buffers inside the Android Keystore cipher, not
  arrays Vigil can reach. A garbage collection removed them.
- Nothing was written to disk unencrypted.

iOS has not been measured. The same sources of copies apply there:
Swift strings, `Data` and the Flutter engine.

## What this means

Vigil erases the copies it owns. It cannot erase the configuration from
the process completely. Someone who can read the memory of the app
process can find a configuration that was imported since the app last
started. On a phone this needs root, malware with that access, or a
debuggable build of the app.

To keep the time a configuration is readable short:

- Restart the app after importing a configuration, it then only uses the
  stored copy.
- Prefer QR codes over pasting. Vigil clears the clipboard after it
  reads a configuration from it, but a keyboard or clipboard manager may
  have kept its own copy.

## Testing this yourself

Run a debug build on an emulator that allows `adb root`, for example a
`google_apis` image. Import a configuration with a secret you generated
for the test. Then scan the readable mappings in `/proc/<pid>/maps` of
the app through `/proc/<pid>/mem` for the secret in all three forms. Do
this after importing, while connected and after disconnecting. Repeat
after a cold start of the app to separate the import path from the
stored path.
