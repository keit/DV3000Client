[日本語版](README-jp.md)

# DV3000Client

A Linux desktop client for digital-voice radio reflectors and networks, built
around a real **ThumbDV** (AMBE3000) USB dongle:

- **D-Star** via DExtra (XLX and XRF reflectors) and DPlus (REF reflectors
  and DPlus repeater gateways)
- **DMR** on two networks: **BrandMeister** via its Open DMR Terminal
  protocol, and **TGIF** via the Homebrew/MMDVM protocol
- **YSF** (Yaesu System Fusion) reflectors, in DN mode

There is no software vocoder here. All AMBE encode/decode is done by the
ThumbDV hardware over serial (through the vendored `serialDV` library), so
you need the dongle to transmit or hear anything.

The main program is a Qt6 GUI (`dv3kclient`) with a tab per protocol.
The repo also builds a few command-line test tools (see below).

**Download:** ready-made `.deb` and AppImage builds for x86_64 PCs and 64-bit
Raspberry Pi, with setup instructions, are at <https://keit.github.io/DV3000Client/>.
On Debian, Ubuntu and Raspberry Pi OS you can also install from the
[keit APT repository](https://keit.github.io/apt/) and get updates with your
system updates. The rest of this README is
about building from source.

## Features

- **D-Star tab:** searchable picker of XLX, XRF and REF reflectors and DPlus
  repeater gateways (live directories), module selection (or **All modules**
  to monitor a whole reflector or repeater), connect/disconnect, push-to-talk,
  your TX message, last-received header and Last Heard list with each
  station's module and TX message.
- **DMR tab:** a network picker (BrandMeister or TGIF), searchable talkgroup
  picker for that network, group calls (plus private calls on BrandMeister),
  current-subscription display, a favourites list per network, talkgroup
  scanning on BrandMeister, Last Heard list with callsign and talkgroup-name
  lookup.
- **YSF tab:** searchable picker of YSF reflectors (live directory; type a
  name or a reflector's number), connect/disconnect, push-to-talk, a DG-ID
  setting for reflectors with rooms, favourites, and a Last Heard list with
  each station's gateway, DG-ID and mode.
- **Volume:** microphone and speaker sliders with level meters at the bottom
  of every tab, adjustable mid-QSO. **Auto level** evens out how loud
  different stations are, and **Record** saves what you hear (see below).
- **Settings:** callsign/DMR identity, the server and password for each DMR
  network, audio input/output device with test buttons (tone for output,
  live level meter for input), ThumbDV serial device.
- Directory data (reflectors, talkgroups, DMR IDs) is cached on disk and
  refreshed at most every 24 hours.

## Requirements

- Linux (developed on Ubuntu; also used on a Raspberry Pi)
- A ThumbDV USB dongle, either plugged into this machine or on another one
  running [AMBEServer 3000](https://www.pa7lim.nl/ambeserver-3000-for-linux/)
  (e.g. a Raspberry Pi), reached over UDP
- An ALSA sound device for microphone and speaker (a USB headset works well)
- C++17 compiler, CMake 3.17+, pkg-config
- ALSA development headers
- Qt6 Widgets development files (only needed for the GUI)

On Ubuntu/Debian:

```sh
sudo apt install build-essential cmake pkg-config git libasound2-dev qt6-base-dev
```

If Qt6 isn't found, CMake skips the GUI and builds only the command-line tools.

## Build

This repository uses git submodules (`serialDV` and `xlxd`), so clone with
`--recurse-submodules`.

```sh
git clone --recurse-submodules git@github.com:keit/DV3000Client.git
cd DV3000Client

cmake -S . -B build
cmake --build build -j"$(nproc)"
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

The build produces these binaries in `build/`:

| Binary               | Purpose                                                             |
| -------------------- | ------------------------------------------------------------------- |
| `dv3kclient`         | The Qt GUI (D-Star, DMR and YSF tabs)                               |
| `dextra_test`        | Command-line DExtra client, incl. live audio mode                   |
| `dplus_test`         | Command-line DPlus client: listens and logs every stream            |
| `ysf_test`           | Command-line YSF client: lists reflectors, listens, transmits a WAV |
| `dmr_test`           | Command-line DMR client for Homebrew/MMDVM masters (e.g. TGIF)      |
| `odt_test`           | Command-line BrandMeister Open DMR Terminal client, with live audio |
| `roundtrip_test`     | PCM to AMBE to PCM round trip through the ThumbDV                   |
| `xlx_directory_test` | Fetches and prints the XLX reflector directory                      |

## One-time system setup

**Serial device access.** The ThumbDV shows up as a serial device. Add yourself
to the group that owns it, then log out and back in:

```sh
sudo usermod -aG dialout "$USER"
```

Find the stable device path (you'll enter it in Settings):

```sh
ls -la /dev/serial/by-id
```

**FTDI latency timer.** The ThumbDV uses an FTDI chip whose default 16 ms
latency timer causes audio glitches (playback xruns, growing backlog). DV3K
Client asks the driver for 1 ms itself each time it opens a ThumbDV plugged
into the same machine, and shows a yellow warning above the tabs if that
didn't work. **You only need the udev rule below if you see that warning.**
If your ThumbDV is behind AMBEServer, the rule goes on the machine running
AMBEServer, not the one running DV3K Client. To set it to 1 ms permanently:

```sh
echo 'ACTION=="add", SUBSYSTEM=="usb-serial", DRIVER=="ftdi_sio", ATTR{latency_timer}="1"' \
  | sudo tee /etc/udev/rules.d/99-ftdi-latency.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug and replug the dongle afterwards. Verify with:

```sh
cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer   # should print 1
```

**Wi-Fi power saving.** On a machine that reaches the network over Wi-Fi,
power saving can hold packets for 100 ms or more when traffic starts after a
pause, which is exactly what pressing PTT does. With the ThumbDV behind an
AMBEServer this breaks up transmit and receive audio, so turn it off on both
ends. Check the current state with `iw dev <interface> get power_save` (find
the interface name with `iw dev`), or with `/sbin/iwconfig` where `iw` isn't
installed (Raspberry Pi OS); it should say `Power Management:off`.

_Pi-Star:_ the interface keeps the kernel name `wlan0` and there is no
NetworkManager, so a udev rule is enough:

```sh
echo 'ACTION=="add", SUBSYSTEM=="net", KERNEL=="wlan*", RUN+="/sbin/iw dev $env{INTERFACE} set power_save off"' \
  | sudo tee /etc/udev/rules.d/70-wifi-powersave-off.rules
sudo reboot
```

_Ubuntu, Raspberry Pi OS (Bookworm or later) and other NetworkManager
systems:_ the udev rule above doesn't work here (Raspberry Pi OS doesn't even
include `iw`). On Ubuntu the interface is renamed (e.g. `wlan0` to `wlp1s0`) before the rule's
command runs, and NetworkManager turns power saving back on when it connects
anyway (Ubuntu ships `default-wifi-powersave-on.conf`). Override it in
NetworkManager instead. The file name must sort after `default-...`, because
the file read last wins:

```sh
printf '[connection]\nwifi.powersave = 2\n' \
  | sudo tee /etc/NetworkManager/conf.d/wifi-powersave-off.conf
sudo systemctl restart NetworkManager
```

**Background Wi-Fi scans.** This applies only when the ThumbDV is on another
machine (AMBEServer) and the machine running DV3K Client reaches it over Wi-Fi. A local serial
ThumbDV, or a wired connection, isn't affected. Every few minutes the Wi-Fi
card scans all channels, which takes the radio off your network's channel
for around 9 seconds. During that time every AMBEServer reply is delayed to
~150 ms, well past the per-frame budget, so you get ~10–20 seconds of broken
audio every few minutes while listening. Short tests often fall between two
scans and look fine. On a GNOME/NetworkManager desktop two separate things
trigger these scans, each about every 5 minutes, and both need turning off. On
Raspberry Pi OS only the second does (there's no geoclue), every 2-3 minutes,
each scan taking it off the channel for about 3 seconds:

1. *The location service (geoclue)*, which scans to work out where you are.
   Turn Location Services off (Settings > Privacy & Security > Location
   Services, or the command below). This is saved per user and survives
   reboots. The trade-off is that features that use your location stop
   working, e.g. Night Light's automatic sunset-to-sunrise schedule (switch
   it to a manual schedule instead).

   ```sh
   gsettings set org.gnome.system.location enabled false
   ```

2. *NetworkManager's roaming scan*, which looks for a better access point.
   It stops when the connection is locked to the access point it's using
   (its BSSID). The trade-off is that the machine no longer moves to another
   access point by itself. That's fine for one that stays put, even on a
   mesh network, but before moving it out of that access point's range, clear
   the lock (`802-11-wireless.bssid ""`) or it won't connect at all, then
   lock it to the new one. A laptop that moves around is better left
   unlocked. Replace `MyWiFi` with your connection's name (`nmcli connection
   show`):

   ```sh
   iw dev wlp1s0 link | head -1    # Ubuntu: "Connected to xx:xx:xx:xx:xx:xx"
   /sbin/iwconfig wlan0 | grep "Access Point"    # Raspberry Pi OS
   sudo nmcli connection modify MyWiFi 802-11-wireless.bssid xx:xx:xx:xx:xx:xx
   sudo nmcli connection up MyWiFi
   ```

To check that nothing is still doing full scans, watch for a few minutes
(Ubuntu; Raspberry Pi OS doesn't include `iw`). A
full scan lists many frequencies on its `scan finished` line; a quick scan
of just your own channel (one frequency) is harmless.

```sh
iw event -t | grep scan
```

## Running the GUI

```sh
./build/dv3kclient
```

To start it from the desktop's application launcher instead (GNOME, KDE,
...), add a launcher entry with its icon for your user. It points at
`build/dv3kclient` in this repo, so run it again if you move the repo;
`--uninstall` removes it.

```sh
./scripts/install-desktop-entry.sh
```

1. Open **File > Settings...** and fill in:
   - **General:** your D-Star callsign, module suffix and optional TX message
     (see [D-Star: REF reflectors and DPlus gateways](#d-star-ref-reflectors-and-dplus-gateways)); for DMR, your DMR
     ID and then, per network, that network's fields. Pick **BrandMeister** or
     **TGIF** in the _DMR network_ box to see and edit its fields. You can
     fill in both; only networks with a server and password set appear on the
     DMR tab. TGIF also has the ID suffix and location/description/URL
     fields. (Frequency, color code and time slot are RF-only, so they're
     fixed in the code rather than settings.)
   - **Devices:** audio input and output devices (use the Test buttons to
     confirm you picked the right ones) and the ThumbDV: either a local
     serial device, or a network host and port (see below).
2. **D-Star tab:** pick a reflector, choose the target module, click
   **Connect**.
3. **DMR tab:** choose the **Network** (while disconnected), click
   **Connect**, pick or type a talkgroup, then use **PTT to send**.
4. **YSF tab:** pick a reflector (and, on a reflector with rooms, set the
   DG-ID; see [YSF](#ysf)), click **Connect**.
5. **PTT to send** toggles transmit on and off (click it, or press the space
   bar when a text field isn't focused).

Settings can't be changed while a session is connected.

### D-Star: REF reflectors and DPlus gateways

REF reflectors and DPlus repeater gateways (a repeater's own callsign, e.g.
`ZL2VH`) are in the same reflector picker as XLX and XRF, labelled _DPlus
reflector_ or _DPlus gateway_. They use DPlus, the protocol DV Dongles and
hotspots use for REF, which works a little differently:

- **Your callsign must be registered for D-Star** (a US Trust/ircDDB
  registration, as for any gateway). Connecting first logs in to the DPlus
  authentication server; that login is also where the list of REF reflectors
  and gateways comes from, so it appears once your callsign is set.
- **Module suffix.** The app logs in, and transmits, as your callsign plus
  the _Module suffix_ letter, e.g. `ZL2MIM P`, the way hotspots do. A DPlus
  server allows one login per name, so pick a letter none of your other radios
  or hotspots use while linked at the same time. If the name is already
  connected (a hotspot, a radio's Terminal Mode or Repeater Monitor), the
  connection is refused with a message saying so. The letter must be one of
  your registered D-Star terminals: otherwise the login is read-only, and the
  status line says _listen only_.
- **All modules** plays whichever module's transmission comes first, like a
  radio's Repeater Monitor, and lists every module's stations in Last Heard.
  It's listen only; pick a module to transmit.
- A repeater gateway passes on CQ calls (UR `CQCQCQ`) only. A station
  routing a call somewhere (UR set to a callsign, or to `/ZL2VH C`) isn't sent
  to DPlus users, so you won't hear it.

### YSF

The YSF tab connects to YSF reflectors, the network Yaesu System Fusion
hotspots and gateways use, picked from the public YSF reflector list. It
uses your callsign from Settings; there's nothing else to set up.

- **DN mode only.** Fusion radios send DN (digital narrow) by default, and
  that's what the app plays and transmits, through the ThumbDV. A station
  sending VW (full-rate voice) still appears in Last Heard, marked "not
  played".
- **DG-ID and rooms.** Plain YSF reflectors ignore the DG-ID (Digital Group
  ID, 00-99), so leave it at 00. Reflectors with rooms (YCS servers, and
  ones such as JAPANLINK) use it to pick the room: connecting puts you back in
  the room you were last in, and keying up moves you to the room of the
  DG-ID box -- on JAPANLINK, 00 is the welcome room. The app remembers the
  DG-ID you last used on each reflector and sets the box when you pick it, so
  your first over keeps you in the room you land in.
- **Testing:** a YSF parrot plays back what you send. ES-PARROT-QB (type
  `00074`) works well.

### ThumbDV on another machine (AMBEServer 3000)

Instead of a local serial device, the dongle can sit on a Raspberry Pi (or any
Linux box) running PA7LIM's AMBEServer 3000, which bridges it to UDP. In
**Settings > Devices**, set _ThumbDV_ to **Network (AMBEServer 3000)**, enter
the host name or IPv4 address and the port (default 2460), and press **Test**
to check that the server answers.

- Only one client can use an AMBEServer at a time, so don't run the GUI and
  a command-line tool against the same server together. A connected GUI counts
  even when idle: it still decodes whatever it receives, so press
  **Disconnect** on any other machine that uses the same server.
- Every 20 ms audio frame is a UDP round trip, so a wired LAN is best. On
  Wi-Fi, turn off power saving on both ends and stop background scans on
  this PC (see _Wi-Fi power saving_ and _Background Wi-Fi scans_ under
  [One-time system setup](#one-time-system-setup)). A reply that takes more
  than 100 ms is treated as lost.
- The command-line tools accept the same thing in place of the serial device,
  e.g. `./build/roundtrip_test 192.168.1.20:2460 in.raw out.raw`.
- **Raspberry Pi USB stalls.** On a Pi 3B+, the ThumbDV's USB link
  occasionally stalls, and the Pi logs
  `ftdi_sio ttyUSB0: usb_serial_generic_read_bulk_callback - urb stopped: -32`
  (`dmesg -T | grep "urb stopped"`). AMBEServer never reopens the port, so
  from then on it doesn't answer at all: the GUI log fills with
  `getResponse: cannot get response` and you hear nothing until AMBEServer is
  restarted. `scripts/pi-star/` has a small watchdog service that follows the
  kernel log and restarts AMBEServer when the stall appears, so you lose a
  second or two of audio instead. Copy that directory to the Pi and run:

  ```sh
  sudo ./install-ambeserver-watchdog.sh
  ```

  It assumes the service is called `ambeserver` (set `AMBESERVER_SERVICE` in
  the unit file otherwise) and remounts Pi-Star's read-only root filesystem
  only for the install. `journalctl -u ambeserver-watchdog` shows each
  restart.

### DMR notes

- The DMR ID is your registered ID. Each network has its own credential:
  BrandMeister wants the **Hotspot Security** password you set in SelfCare
  (not your account password); TGIF wants the **Hotspot Security Key** (16 digits) generated on
  your TGIF account's security page.
- Servers are just hostnames, and the port is fixed per network -- 62031 for
  TGIF's Homebrew, 54006 for BrandMeister's Open DMR Terminal -- so don't add
  one. BrandMeister's server box is a dropdown of its masters ("AU 5051",
  "DE 2621", ...), fetched from BrandMeister and cached for a day; you can
  also type a hostname such as `3101.master.brandmeister.network`.
- **BrandMeister (Open DMR Terminal)** only delivers what you're subscribed
  to. The client subscribes to the talkgroup in the box when you press PTT,
  not when you connect; **Unsubscribe** (next to _Current
  subscription_) drops it, and disconnecting unsubscribes automatically. For a
  **private call** (e.g. the Parrot echo test, ID 9990), tick **Private
  call**; the talkgroup field is then a target DMR ID.
- **Scanning** (BrandMeister): tick the favourite talkgroups to scan and press
  **Scan** -- the button reads **Scanning** -- to listen to all of them at once.
  BrandMeister passes one call at a time, so calls don't overlap. PTT is off
  while scanning, so you never transmit on a talkgroup you didn't mean to. To
  reply, press **Scanning** again: the talkgroup of the last call you heard
  goes in the talkgroup box, ready for PTT. Double-clicking a call in Last
  Heard does the same for that call's talkgroup.
- **TGIF (Homebrew)** has no separate subscription step: you hear whichever
  talkgroup you last transmitted on, which is what _Current subscription_
  shows. TG 4000 is a "landing place" that passes no traffic (shown as
  "None"). TGIF doesn't support private calls, so that checkbox is disabled;
  test with a **group** call to TG 9990 or 31000 (Parrot).
- **DMR ID suffix** (TGIF): to run this client alongside another hotspot under
  the same DMR ID, enter a 2-digit suffix. It's appended to form a unique
  9-digit ID (TGIF's "ESSID"). Leave it blank to use the plain 7-digit ID.
- Talkgroup numbers mean different things on different networks, so each
  network has its own talkgroup list and its own favourites.

### Files the GUI writes

Under `~/.config/DV3000Client/`:

| File                       | Contents                                                        |
| -------------------------- | --------------------------------------------------------------- |
| `settings.json`            | Your settings (includes the DMR hotspot password in plain text) |
| `dmr_favourites.json`      | BrandMeister favourites list                                    |
| `dstar_favourites.json`    | D-Star favourites (reflector + module, or all modules)          |
| `dmr_favourites_tgif.json` | TGIF favourites list                                            |
| `ysf_favourites.json`      | YSF favourites (reflector names)                                |
| `ysf_dgids.json`           | The DG-ID last used on each YSF reflector                       |
| `dv3000client.log`         | Log file (also reachable via **Help > Log File Location...**)   |
| `cache/`                   | Cached reflector, talkgroup and DMR ID directories              |

**Your own reflector entries.** To reach an XLX/XRF reflector at an address
other than the directory's -- your own reflector from inside its network, at
its private address -- or one that isn't listed at all, create
`~/.config/DV3000Client/reflectors.xml` with entries in the directory's own
format. An entry replaces the directory's reflector of the same name, and
any other is added; `<lastip>` can be a host name, `<country>` and
`<comment>` are optional. The app reads it at startup and marks its entries
"(reflectors.xml)" in the list.

```xml
<reflectorlist>
  <reflector>
    <name>XLX307</name>
    <lastip>192.168.1.50</lastip>
    <comment>at home</comment>
  </reflector>
</reflectorlist>
```

YSF reflectors work the same way, in `~/.config/DV3000Client/ysf_reflectors.txt`
with lines in the YSF list's own format (Pi-Star's `YSF_Hosts.txt`):
number, name, description, address (or host name) and port, each followed by
`;`. Lines starting with `#` are comments. Entries are marked
"(ysf_reflectors.txt)" in the list.

```
# number;name;description;address;port;
99999;MY-YSF;at home;192.168.1.60;42000;
```

The **Record** button in the Speaker box saves what you hear as WAV files in
`~/Music/DV3K Client/`, named by date, time and what you're connected to
(**File > Open Recordings Folder** opens it). Your own transmissions aren't
included.

## Command-line tools

Examples; run any tool with no arguments for its usage line.

```sh
# D-Star: link to a reflector on module B and go live with audio devices
./build/dextra_test <reflector-ip> B /dev/serial/by-id/<thumbdv> live plughw:1,0 plughw:1,0

# D-Star DPlus: log in as "<callsign> P" and log every stream on REF001, all modules,
# without decoding (no ThumbDV or audio device needed)
./build/dplus_test <callsign>/P REF001 none none all

# YSF: list reflectors matching "parrot", then listen to one and save what's
# heard to a WAV file; --tx sends a recording (8 kHz 16-bit mono WAV) first
./build/ysf_test --list parrot
./build/ysf_test <callsign> 00074 /dev/serial/by-id/<thumbdv> heard.wav --tx myvoice.wav

# DMR (Homebrew, e.g. TGIF): log in and send a short test transmission to the
# Parrot (group call to TG 9990), staying connected 20 seconds. The password
# "-" reads it from $DMR_PASSWORD, so it never lands in shell history.
read -s "DMR_PASSWORD?TGIF Hotspot Security Key: "; export DMR_PASSWORD
./build/dmr_test --suffix 01 tgif.network 62031 <dmrId> - <callsign> 20 9990

# DMR (BrandMeister Open DMR Terminal): connect with real audio, subscribe to
# TG 9990(parrot TG) to do echo test.
read -s "BM_PASSWORD?BM Hotspot Security Key: "; export BM_PASSWORD
./build/odt_test <master-host> <dmrId> "$BM_PASSWORD" /dev/serial/by-id/<thumbdv> plughw:0,0 plughw:1,0 9990 private
```

```sh
# List of BrandMeister masters, useful for picking the right hostname
https://brandmeister.network/#/masters
```

```sh
# aplay & arecord list devices, useful for picking the right ALSA device names
aplay -l
arecord -l
```

### Local test reflector

To test D-Star without touching the public network, build and run a local
`xlxd` reflector from the vendored submodule:

```sh
./scripts/run-test-xlxd.sh XLX999 127.0.0.1     # listen on 0.0.0.0 to reach it from another machine
```

`xlxd` is vendored unmodified; the script applies a small local-testing patch
(`scripts/xlxd-local-test.patch`) at build time.

## Project layout

```
src/                  protocol clients, audio, DMR voice/FEC, CLI tools
src/gui/              Qt GUI (main window, D-Star, DMR and YSF tabs, settings)
data/                 static fallback reflector list
resources/            application icons (from the 1024px original) and Qt resource file
scripts/              local xlxd test-reflector helper, desktop launcher install
scripts/pi-star/      AMBEServer watchdog for a Pi-Star / Raspberry Pi
third_party/serialDV  ThumbDV/AMBE3000 serial driver (fork, submodule)
third_party/xlxd      xlxd reference code (submodule; DMR and YSF FEC code is reused)
```

## Acknowledgements

- [serialDV](https://github.com/f4exb/serialDV) by F4EXB for the ThumbDV
  serial interface (this repo uses a fork)
- [xlxd](https://github.com/LX3JL/xlxd) by LX3JL, whose D-Star/DMR/YSF code
  informed the protocol implementations and whose FEC routines are reused

## License

This project is licensed under the GNU General Public License v3.0 (GPLv3) - see the [LICENSE](LICENSE) file for details.
Includes code from `serialDV` (GPLv3) and DMR FEC encoders from `xlxd`
(GPLv2 or later). xlxd's GPLv2-only `cutils` helper is not used; `src/dmr_fec`
has our own replacement.
