# Vigil

An ISC licensed Sanctum "companion" app for iOS and Android.

I call it companion because it's not just "VPN".

Vigil supports talking to the Sanctum daemon (https://sanctorum.se)
using one of the following modes:

- Normal tunnels (client-to-site)
- Cathedral tunnels (device-to-device with NAT holepunching)
- Cathedral liturgies (autodiscovery of cathedral tunnels in full-mesh)

All of the above are L3 based networks.

Vigil also supports talking to the sanctum-based tier6 daemon
(https://github.com/jorisvink/tier6) giving you the possibility
to join your phone into an L2 network.

On top of that Vigil also implements the sanctum confessions
(https://github.com/jorisvink/confessions) and gospel protocols
(https://github.com/jorisvink/gospel) to support the following:

- Voice calls
- Video calls
- Text messaging
  - Including the ability to send images or files.

# Disclaimer

I wrote this project with the help of LLMs. The security critical
bits and pieces are entirely written by me, while all other code
has been reviewed before being accepted.

I rewrote a bunch of code the LLM suggested as well as it was not
up-to-par with what I consider good code.

I know nothing about app development, I am an old unix neckbeard who
lives in dark moody terminal, so this was somewhat of a benchmark on how
quicky and correctly I could get something like this off the ground.
The results are staggering and I think LLM-assisted development is the
way forward for projects like this.

I will continue to refuse LLM written code for security critical pieces
of software such as Sanctum and libkyrka. I strongly believe that those
should still be 100% human driven. Even so, LLMs should always be used
in the hunt for security bugs, as another tool in your toolbox next
to your fuzzers.

Building a strong foundation first, and designing things from scratch
by yourself will allow one to make good use of LLM driven development
once the basic building blocks are in place.

## Building

### Android

Make sure Android Studio is installed. Can be compiled on Linux or macOS.

```
$ make android
$ adb install -r app/build/app/outputs/flutter-apk/app-release.apk
```

### iOS

Requires macOS with Xcode installed. Before building make sure you
set your DEVELOPMENT_TEAM in app/ios/Team.xcconfig and setup a profile
with the correct entitlements.

```
$ echo "DEVELOPMENT_TEAM = <team>" > app/ios/Team.xcconfig
$ make ios
$ xcrun devicectl list devices
$ xcrun devicectl device install app --device <device> \
    app/build/ios/iphoneos/Runner.app
```

## Running

### Android

On android you may have to exempt the app from aggressive battery
optimizations and allow it to run in the background. Some vendors
will still VPN like applications.

Consider turning on always-on VPN so its automatically restarted too.

# Configuration

## Examples

### A standard tunnel config

```
vigil 1
instance office
tunnel 10.77.0.2/24 1408
peer vpn.example.com:4500
secret <64 hex characters>
route 0.0.0.0/0
dns 10.77.0.1
```

### A tunnel using a cathedral

```
vigil 1
instance laptop
tunnel 10.88.0.2/24
spi 0c0d
kek <64 hex characters>
cathedral cathedral.reliquary.se:4469
cathedral_id 5eb2411f
cathedral_flock 493abf95a07e0c00
cathedral_secret <64 hex characters>
cathedral_cosk <128 hex characters>
```

### A liturgy via a cathedral

```
vigil 1
instance mesh
mode liturgy
spi 0c
kek <64 hex characters>
liturgy_group 0001
liturgy_prefix 172.31.0.0
cathedral cathedral.reliquary.se:4469
cathedral_id 5eb2411f
cathedral_flock 493abf95a07e0c00
cathedral_secret <64 hex characters>
cathedral_cosk <128 hex characters>
```

### A tier6 configuration

```
vigil 1
instance flock
mode liturgy
tier6 yes
spi 0c
kek <64 hex characters>
cathedral cathedral.reliquary.se:4469
cathedral_id 5eb2411f
cathedral_flock 493abf95a07e0c00
cathedral_secret <64 hex characters>
cathedral_cosk <128 hex characters>
```

Do not specify a tunnel ip if you want tier6 to auto-assign addresses.

### A gospel configuration

```
vigil 1
instance calls
mode gospel
spi 0c
kek <64 hex characters>
liturgy_group 0001
cathedral cathedral.reliquary.se:4469
cathedral_id 5eb2411f
cathedral_flock 493abf95a07e0c00
cathedral_secret <64 hex characters>
cathedral_cosk <128 hex characters>
```

## Common options

Other things that can be configured on mostly all configurations:

- shroud (turn shroud off / on)
- dns (configures the DNS to use)
- mtu (set to auto for MTU discovery)
- route (add explicit routes over the tunnel)
- block_ipv6 (defaults to yes when you have a 0.0.0.0/0 route)

## Generating a QRcode

You can use the tools/vigil-config shell script to take existing
files from an existing Reliquary configuration and convert them
into a QRcode or plaintext configuration.

```
$ ./tools/vigil-config cathedral -i laptop -c cathedral.reliquary.se:4469 \
    -f 493abf95a07e0c00 -x 0c0d -k kek-0x0c -I 5eb2411f \
    -C id-5eb2411f -K cosk-5eb2411f -t 10.88.0.2/24 -q
```

Note: the QRcode contains the keys, they should be treated as such.
