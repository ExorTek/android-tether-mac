# android-tether-mac

A macOS driver for USB tethering from an Android phone, built as a
DriverKit system extension.

Plug the phone in, turn on USB tethering, and it appears as an Ethernet
interface. No kernel extension, no disabling SIP, no lowering the boot
security policy on Apple Silicon.

> [!IMPORTANT]
> **This driver has never been run against a phone, and it cannot be
> until Apple grants three restricted entitlements.** What exists today
> compiles, links, signs, and has its protocol layer under test. That is
> not the same as working. See [Status](#status).

## What it is

Android phones present USB tethering as RNDIS — a Microsoft protocol that
carries Ethernet frames over two USB bulk endpoints, with a control
channel on endpoint 0. Windows has had a driver since forever, Linux has
had one for twenty years, and macOS has never shipped one.

This project is an independent implementation of the host side, written
from the published specification:

- **[MS-RNDIS]**, Microsoft's Remote Network Driver Interface
  Specification, for the message layouts, OIDs and status codes.
- **Apple's NetworkingDriverKit and USBDriverKit documentation**, and the
  *Connecting a network driver* sample, for the driver lifecycle.
- **USB-IF class codes** for the interface triples it matches on.

No third-party driver source is included or adapted. See [NOTICE](NOTICE).

## Status

| | |
|---|---|
| RNDIS protocol layer | written, 33 tests / 86 checks, sanitizers clean |
| System extension | compiles, links, signs |
| Installer app | compiles, links, signs |
| Loading on a Mac | **blocked on Apple** — the entitlements are restricted |
| Tested against a phone | **no** |

### What Apple has to approve

A DriverKit extension that touches the network stack and USB needs three
entitlements, and none of them can be self-assigned:

- `com.apple.developer.driverkit`
- `com.apple.developer.driverkit.family.networking`
- `com.apple.developer.driverkit.transport.usb`

They are requested together, as one group, through the DriverKit
entitlement request form against an App ID. Until Apple grants them, a
signed build is refused at load time — not at build time, which is why CI
can be green while nothing can actually be installed.

One awkward detail, recorded here rather than discovered later: the
request form asks which **hardware vendor ID** the driver is for. This
driver does not have one. It matches on the USB interface class that
every Android RNDIS gadget presents, across every manufacturer, which is
the shape of request the form is least set up for.

## Requirements

- macOS 13 or later
- Apple Silicon (the extension is built arm64e; a plain arm64 build
  stages, activates, and then fails to launch)
- An Android phone with USB tethering

## Installing

Once there are signed releases, installation is: unzip, drag
**AndroidTether.app** to `/Applications`, open it, click **Install
Driver**, and approve the extension in **System Settings › Privacy &
Security**.

The app has to stay in `/Applications`. macOS deactivates a system
extension whose host app is gone, so deleting the app is also how you
uninstall the driver.

## Building

Needs the **full Xcode**, not just the Command Line Tools: `iig` and the
DriverKit SDK ship only with Xcode.

```bash
make test
```

```bash
make
```

`make test` builds and runs the protocol tests and needs nothing but
clang — it runs on Linux, which is where CI runs it. `make` additionally
builds the extension and the app into `driver/build/`.

To build signed, drop a DriverKit provisioning profile next to
`driver/Makefile` (the Team ID is read out of it, never committed) and
pass your identity:

```bash
make -C driver SIGN_ID="Developer ID Application: NAME (TEAMID)"
```

To run an unapproved build during development, Apple's developer mode
relaxes the signing requirement:

```bash
systemextensionsctl developer on
```

## How it works

```
rndis/      the protocol, in plain C: no IOKit, no DriverKit, no kernel
tests/      host-side tests for all of the above
driver/     the system extension
app/        the installer that carries it
```

The split is the point. Everything a phone can influence — message
lengths, buffer offsets, record counts — is decided in `rndis/`, which
has no platform in it and can therefore be fed the shapes real hardware
will not produce on request. The driver moves bytes between USB pipes and
the network stack and makes no protocol decisions of its own.

### Why that matters

Every length and offset in a received transfer is chosen by the device on
the other end of the cable. A phone — or something pretending to be one —
can claim a frame that starts past the end of the buffer it arrived in,
or an offset and a length that sum to less than either because 32-bit
arithmetic wrapped. The tests are mostly those cases:

```
truncated header, msg_len past the transfer, data_offset past the record,
offset+length overflow, a frame overlapping its own header, zero-length
frames, frames larger than the receive buffer, control messages arriving
on the data pipe
```

All bounds arithmetic widens to 64 bits before comparing, and the parser
stops at the first record that does not make sense while still delivering
the good frames that came before it.

### The driver

One dispatch queue handles everything: both pipe completions, the
transmit doorbell, and the retry timer. That is what makes the absence of
locking correct rather than lucky.

Receive uses four buffers in flight so the device always has somewhere to
put the next transfer. Transmit copies each frame into its own buffer
behind the RNDIS header — the header has to go *in front* of the frame,
and the packet handed over by the network stack has no room there — which
also means the stack's packet can be returned immediately instead of
being held until USB completes.

The RNDIS packet filter is set in `SetInterfaceEnable` rather than at
startup, so a phone that is plugged in but whose interface nobody brought
up is not asked to forward traffic.

### Known uncertainties

Three decisions in the driver are reasoned rather than copied from a
worked example, and they are the first places to look if it misbehaves:

1. **Link header length on receive** is set to 14, the Ethernet header.
   Apple's sample passes 0, but its packet is a synthetic IP datagram
   with no link header at all; ours is a real Ethernet frame.
2. **Queue order in `RegisterEthernetInterface`** is RX submission, RX
   completion, TX submission, TX completion. The documentation does not
   state an order.
3. **No keep-alive.** Android's gadget does not require one, and a timer
   issuing synchronous control transfers from the datapath queue is a
   hazard for no benefit. If a device turns out to halt an idle session,
   this is what to add.

## Contributing

Bug reports are most useful with the system log (`log show --predicate
'eventMessage CONTAINS "AndroidTether"' --last 5m`) and the output of
`system_profiler SPUSBDataType` for the phone.

Changes to `rndis/` should come with tests; that is the whole reason it
is a separate, platform-free directory.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
