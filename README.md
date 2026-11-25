# android-tether-mac

A macOS driver for USB tethering from an Android phone, built as a
DriverKit system extension.

Plug the phone in, turn on USB tethering, and it appears as an Ethernet
interface. No kernel extension, no disabling SIP, no lowering the boot
security policy on Apple Silicon.

> [!IMPORTANT]
> **The extension itself has never been loaded, and cannot be until
> Apple grants three restricted entitlements.** In the meantime,
> [`android-tether`](#using-it-today-android-tether) gets a Mac online
> through the phone *today*, from user space -- as an app with a Connect
> button, or as a command with `sudo`. See [Status](#status).

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
| DHCP, ARP and IPv4 for the bridge | written, 16 tests / 47 checks, sanitizers clean |
| Protocol against a phone | **works** — Xiaomi 15T Pro, via [`rndis-probe`](#testing-the-protocol-against-your-phone) |
| Internet through a phone | **works**, from user space — [`android-tether`](#using-it-today-android-tether), ~96 Mbit/s on mobile data |
| The app's Connect button | **works** against the same phone -- see [the app](#the-app) |
| System extension | compiles, links, signs |
| The app's Install Driver button | compiles, links, signs |
| Loading on a Mac | **blocked on Apple** — the entitlements are restricted |
| Extension against a phone | **no** |

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

| | |
|---|---|
| **macOS** | 13 Ventura or later |
| **Mac** | Apple Silicon or Intel |
| **Phone** | Any Android device with USB tethering |

macOS 13 is the floor because the extension targets DriverKit 22. Going
lower is possible on paper -- NetworkingDriverKit dates to macOS 11 --
but nothing below 13 has been built or tested, so the number here is the
one that is actually true.

There is no upper bound: a DriverKit extension is not tied to the macOS
version it was built against.

### Architectures

Releases ship one disk image per kind of Mac:

| Your Mac | Download | Extension | App |
|---|---|---|---|
| Apple Silicon | `...-AppleSilicon-arm64e.dmg` | `arm64e` | `arm64` |
| Intel | `...-Intel-x86_64.dmg` | `x86_64` | `x86_64` |

Building locally gives you a universal binary covering both, which is
the default and is what CI checks; the split is only in what gets
published.

The extension's Apple Silicon slice must be **arm64e**, not plain
`arm64`. A plain `arm64` dext stages and activates and then fails to
launch, with a message that does not explain why. The app has no such
requirement -- arm64e is a DriverKit rule, not an application one.

The build asks the installed SDK which architectures it can still
produce rather than assuming both, so when Apple eventually drops
`x86_64` from the DriverKit SDK this builds an Apple-Silicon-only
extension instead of failing.

## Installing

There are three ways in, and they all end at the same bridge:

| | For | What it asks of you |
|---|---|---|
| [The app](#the-app) | anyone | drag to Applications, click Connect, type your password |
| [The command](#the-command-line) | people who live in a terminal | `sudo android-tether` |
| [From source](#building) | people who want to read it first | `make tether`, or `make` for the app |

The driver is a fourth, and the one this project is named for; it is
[not installable yet](#what-apple-has-to-approve). When it is, the same
app installs it: click **Install Driver** and approve the extension in
**System Settings › Privacy & Security**. The app then has to stay in
`/Applications` -- macOS deactivates a system extension whose host app is
gone, so deleting the app is also how you uninstall the driver.

## Using it today: `android-tether`

Until Apple grants the entitlements, there is a way to get online through
the phone that needs none of them.

### The app

1. From a [release](../../releases), download the disk image for your
   Mac: `AppleSilicon-arm64e` for M1 and later, `Intel-x86_64` otherwise.
2. Open it and drag **AndroidTether.app** onto the Applications folder
   beside it.
3. Open the app. The first time, macOS refuses: the build is not signed
   with an Apple Developer ID, which costs $99 a year this project does
   not spend. Go to **System Settings › Privacy & Security**, scroll
   down, and click **Open Anyway**. Once is enough.
4. Plug the phone in. On the phone: **Settings › Hotspot & tethering ›
   USB tethering**, on. (The name and place vary between manufacturers.)
5. Click **Connect**, and type an administrator's password when macOS
   asks.

The window then says *Connected through the phone*, with the interface,
the address the phone gave it and how long it has been up, and two
panels showing download and upload speed and the totals so far.
**Details** opens everything the bridge has printed -- selectable, so it
can be pasted into a bug report -- and, below that, the driver.
**Disconnect**, closing the window, or quitting all put the Mac back as
it was.

The password is asked for every time. Creating a network interface
needs root, and the only way for an app to keep that right between runs
is a privileged helper, which macOS installs only from a Developer
ID-signed app. So this one asks, each time, through the system's own
password window, and runs the `android-tether` inside its bundle.

### The command line

From source, with the full Xcode or just the Command Line Tools:

```bash
make tether
sudo make install       # optional: /usr/local/bin/android-tether and rndis-probe
```

`sudo make uninstall` takes them out again; `PREFIX=/somewhere/else`
moves them. Installing is worth doing for one reason beyond convenience:
the copy in `/usr/local/bin` belongs to root, where the one in the build
directory can be replaced by anything running as you -- and it is going
to be run as root.

Or from a [release](../../releases): `AndroidTether-tools-<version>.zip`
holds both tools as universal binaries. Until releases are signed with a
Developer ID, a downloaded copy has to have its quarantine lifted before
macOS will run it:

```bash
xattr -d com.apple.quarantine android-tether rndis-probe
```

Then, with the phone plugged in and USB tethering on:

```bash
sudo android-tether         # or ./tools/android-tether/build/android-tether
```

It opens the phone's RNDIS interface the way
[`rndis-probe`](#testing-the-protocol-against-your-phone) does, leases an
address from the phone over DHCP, and bridges a **utun** interface -- the
kind VPN clients use -- to it:

```
ok    sending up to 10 frames per transfer
ok    DHCP lease 10.177.117.33 via 10.177.117.42, DNS 10.177.117.42, 3599 s
ok    interface utun4
ok    routes and DNS point at the phone

tethering through the phone on utun4. Ctrl-C to stop.
```

After that it prints a line of counters every five seconds while traffic
is moving: frames and megabytes in each direction, and how many frames
are sharing a transfer on the way out. The line grows an `errors` part
only when something has gone wrong -- failed transfers in either
direction -- and that part is what to include in a bug report.

**To stop:** Ctrl-C, or pull the cable, or turn tethering off on the
phone. All three leave the Mac as it was.

| Option | |
|---|---|
| `--seize` | Ask a kernel driver that already holds the interface to let go. Only needed if opening the interface fails and names an owner. |
| `--no-aggregate` | Send one frame per USB transfer even if the phone offers more. Slower; for ruling aggregation out when a phone misbehaves. |

### What it changes on the Mac, and what it does not

While it runs, the Mac's IPv4 traffic and DNS go through the phone.
Wi-Fi is not touched; it simply stops being the primary connection, and
becomes it again on Ctrl-C, when the cable is pulled, or if the process
dies -- the interface and its routes belong to the process, and the
configd entry that makes it primary is a session key that goes with it.
Nothing is written to disk and no setting is changed that outlives the
process.

**IPv6 does not go through the phone.** The bridge is IPv4 only, so if
Wi-Fi is on and has IPv6, connections to IPv6-capable sites can still
leave over Wi-Fi. Turn Wi-Fi off if the point is for *everything* to go
through the phone -- to measure it, or because the Wi-Fi is the thing
you do not trust.

If the phone is itself on Wi-Fi, it shares that instead of mobile data,
and the public address you see is the Wi-Fi's.

### Speed

Measured on a Xiaomi 15T Pro over mobile data: ~12 MB/s (about 96
Mbit/s) down. That figure predates the transmit path described below
and has not been re-measured since.

Towards the phone, frames are written straight from the utun into a
transfer buffer and sent asynchronously, several to a transfer when the
phone says it accepts that (`MaxPacketsPerTransfer` in its INITIALIZE
reply; 10 on the phone above). A lone packet still leaves at once:
frames share a transfer only when they were already waiting. When every
buffer is in flight the bridge stops reading the utun, so the queue
builds in the kernel where TCP can see it, rather than being dropped
here.

### What to trust it with

It runs as root, and it reads what a phone sends it. Those two facts are
why the code is laid out the way it is:

- Every length and offset that arrives over the cable -- RNDIS records,
  IPv4 headers, DHCP options, ARP -- is parsed in plain C in `rndis/` and
  `tools/android-tether/netproto.c`, checked against the bytes that
  actually arrived, and tested under AddressSanitizer and
  UndefinedBehaviorSanitizer with the shapes a cooperative phone would
  never send.
- The only programs it runs are `/sbin/ifconfig` and `/sbin/route`, by
  absolute path, with arguments it formats from numbers. No shell.
- It listens on nothing and opens no ports.
- The app talks to it over one Unix socket, which the app creates in
  your own temporary directory and the bridge connects to. The bridge,
  as root, never creates or writes a file at a path the app chose.

One thing the app cannot offer that `sudo make install` can: the
`android-tether` inside `AndroidTether.app` belongs to you, not to root,
so another program running as you could replace it and wait for you to
type your password. That is true of every app that asks for an
administrator's password without a signed privileged helper. If it
matters to you, use the installed command instead.

What it cannot protect you from is the phone itself. The phone is the
router: it sees the traffic, it names the DNS servers, and the Mac
believes it, exactly as it would believe a Wi-Fi access point. Tether to
a phone you trust.

### How it differs from the extension

- **It needs root every time** -- a password in the app, `sudo` in a
  terminal -- and runs until stopped. It does not start by itself when
  the phone is plugged in.
- **The interface is a utun, not an Ethernet interface.** It does not
  appear in System Settings as a network service, and anything that
  insists on an Ethernet interface will not see it.
- **IPv4 only.** IPv6 frames from the phone are counted and dropped.

What it does share with the extension is everything the phone can
influence: the same `rndis/` code reads every transfer.

### When it does not work

The lines below appear in the terminal, or in the app's log.

| What you see | What it means |
|---|---|
| The app will not open: "Apple could not verify..." | The build is unsigned. System Settings › Privacy & Security › **Open Anyway**, once. |
| `FAIL  no RNDIS control interface ...` | The phone is not plugged in, or USB tethering is off. The list printed underneath is every USB interface the Mac can see. |
| `02/0D/00` in that list | The phone uses CDC NCM, not RNDIS. macOS drives that itself: look for the phone under System Settings › Network. This tool is not needed. |
| `FAIL  opening the control interface`, with `is held by ...` | Something else has the interface open -- often another copy of this tool. Stop it, or try `--seize`. |
| `no DHCPOFFER` five times | The phone accepted the handshake but is not handing out addresses. Turn USB tethering off and on again. |
| `the phone went away` | The cable came out, or tethering was switched off. Run it again once it is back. |
| Online, but slow or stalling | Run with `--no-aggregate`. If that fixes it, the phone's output from `rndis-probe` is a bug report worth filing. |

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

| | |
|---|---|
| `make test` | the protocol tests; `make asan` runs them under the sanitizers |
| `make driver` | the extension and the app, with `android-tether` inside it, into `driver/build/` |
| `make tether` | `android-tether`, into `tools/android-tether/build/` |
| `make probe` | `rndis-probe`, into `tools/rndis-probe/build/` |
| `sudo make install` | both tools into `/usr/local/bin` (`PREFIX` to change) |
| `make clean` | everything above, gone |

The two tools need only the macOS SDK, so the Command Line Tools are
enough for `make tether` and `make probe`.

### Releases

The **Release** workflow builds everything on a clean runner and
publishes it: a disk image of the app per architecture, and
`AndroidTether-tools-<version>.zip` with the two command-line tools as
universal binaries. Run it from the Actions tab with a version number,
or push a `v*` tag. Without signing secrets the result is unsigned, and
its notes say what that costs whoever downloads it; with them (listed
at the top of `.github/workflows/release.yml`) everything is signed and
notarized.

To build signed, drop a DriverKit provisioning profile next to
`driver/Makefile` (the Team ID is read out of it, never committed) and
pass your identity:

```bash
make -C driver SIGN_ID="Developer ID Application: NAME (TEAMID)"
```

### What an unsigned build can and cannot do

An ad-hoc build is signed **without the app's entitlements**, on purpose.
Those entitlements are restricted ones that only a provisioning profile
can authorise, and signing a binary with entitlements it is not
authorised for does not merely fail to grant them: AMFI refuses to
launch it at all, before any code runs, with

```
Launch failed ... Launchd job spawn failed (POSIX error 163)
```

Left off, the app starts, and **Connect** works: the bridge needs no
entitlements at all. What an unsigned build cannot do is the other
button; clicking **Install Driver** reports

```
Missing entitlement com.apple.developer.system-extension.install
```

because installing a system extension is exactly the thing that
entitlement authorises.

That one is **not** the hard part. It is an ordinary capability any
Apple Developer Program member can switch on for an App ID, so a $99
membership gets you past this message. What waits immediately behind it
is the real wall: the extension itself still needs the three restricted
DriverKit entitlements, and those Apple grants case by case. Getting
past this error means reaching the next one, not reaching a working
driver.

A build downloaded from Releases meets one more thing first: macOS
quarantines downloaded files, and Gatekeeper refuses an app that is not
notarized ("Apple could not verify ... is free of malware") until you
choose **Open Anyway** in System Settings › Privacy & Security. That
gets you the app and its Connect button. Nothing you can do to the
download makes the driver load.

### Testing the protocol against your phone

Most of what could be wrong with this driver can be found without
loading it. macOS attaches no driver of its own to an Android RNDIS
interface, so an ordinary program can open it and do exactly what the
extension does at start-up -- the same control requests, in the same
order, through the same `rndis/` code. No entitlements, no Developer
account, no SIP changes:

```bash
make probe
./tools/rndis-probe/build/rndis-probe
```

with the phone plugged in and USB tethering on. It initialises the
device, reads its MAC address, sets the packet filter, sends a
DHCPDISCOVER and listens for the answer, printing `ok` or `FAIL` for
each step:

```
ok    control interface 0: E0/01/03 (RNDIS over CDC ACM)
ok    INITIALIZE: RNDIS 1.0, max transfer 15800, 10 packets per transfer, alignment 2^0
ok    packet filter 0x0000000d
ok    sent DHCPDISCOVER, xid 0x1b5aa416, 386 bytes on the wire
ok    DHCPOFFER 10.177.117.33 from 10.177.117.42
PASSED: 0 failures
```

A DHCPOFFER back means both directions work: the phone parsed a record
`rndis_build_packet_header` wrote, and `rndis_parse_transfer` found the
frame in what it sent back. What this cannot tell you is anything
NetworkingDriverKit decides -- the three [known
uncertainties](#known-uncertainties) below are all on that side.

If it finds nothing, it lists every USB interface it can see. `02/0D/00`
is CDC NCM, which some newer phones use instead of RNDIS; macOS drives
that itself, and such a phone does not need this driver at all.

Output from phones other than the one in the Status table is the most
useful bug report this project can get.

### Testing it without an Apple Developer account

An Apple Developer Program membership buys the ability to ship this to
*other people*. Proving the driver works on **your own** Mac does not
need one -- but it does need you to turn off the protections that are
refusing it, which is a real cost and worth understanding before you
start.

With AMFI disabled, entitlements are not checked at all, so the
unauthorised ones stop mattering. The build keeps them on request:

```bash
make -C driver ADHOC_ENTITLEMENTS=1
```

That app will **not** launch on a normal Mac -- it is for a machine
prepared like this:

1. **Apple Silicon only:** shut down, hold the power button until
   "Loading startup options", then Options → Utilities → Startup
   Security Utility → pick your disk → Security Policy → **Permissive
   Security**.
2. In recoveryOS, Utilities → Terminal: `csrutil disable`
3. Boot normally, then
   `sudo nvram boot-args="amfi_get_out_of_my_way=0x1"` and reboot.
4. `systemextensionsctl developer on`

To undo it afterwards: `sudo nvram -d boot-args`, `csrutil enable` from
recoveryOS, and set the security policy back to **Full Security**.

> [!WARNING]
> This disables System Integrity Protection and code-signing
> enforcement for the whole machine, not just for this driver. Every
> program you run loses those protections. It is reasonable on a
> machine you develop on and a bad idea on one you keep your life in.

Two honest caveats. First, this has **not been verified** -- nobody has
run this driver on a prepared machine yet, so these steps are the known
procedure, not a tested recipe. Second, it gets *you* to a working
driver at best; it does nothing for anyone else, since they would have
to lower their own machine the same way. Shipping still needs Apple.

## How it works

```
rndis/      the protocol, in plain C: no IOKit, no DriverKit, no kernel
tests/      host-side tests for all of the above
driver/     the system extension
app/        the app: a Connect button for android-tether, and the
            installer that carries the extension. make-icon.m draws
            AppIcon.icns (`make -C driver icon`)
tools/      rndis-probe: the extension's handshake, from user space
            android-tether: internet through the phone, from user space
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

Measured on the first phone, and consistent with what the driver
assumes: the device offers a max transfer of 15800 bytes, under the
16384 the receive buffers are sized for; it will pack up to 10 frames
into one transfer, which is why `rndis_parse_transfer` loops; the
address `OID_802_3_PERMANENT_ADDRESS` returns is the one the host side
should use, and differs from the phone's own; and a second INITIALIZE
after HALT is accepted, which is what a replug looks like.

## Contributing

Bug reports are most useful with the system log (`log show --predicate
'eventMessage CONTAINS "AndroidTether"' --last 5m`) and the output of
`system_profiler SPUSBDataType` for the phone.

Changes to `rndis/` should come with tests; that is the whole reason it
is a separate, platform-free directory.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
