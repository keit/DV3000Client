# DV3000Client

A Linux desktop client for digital-voice radio reflectors and networks, built
around a real **ThumbDV** (AMBE3000) USB dongle:

- **D-Star** via the DExtra protocol (XLX / XRF-style reflectors)
- **DMR** via the Homebrew/MMDVM protocol (BrandMeister and other masters)

There is no software vocoder here. All AMBE encode/decode is done by the
ThumbDV hardware over serial (through the vendored `serialDV` library), so
you need the dongle to transmit or hear anything.

The main program is a Qt6 GUI (`dv3000client_gui`) with a tab per protocol.
The repo also builds a few command-line test tools (see below).

## Features

- **D-Star tab:** searchable reflector picker (live XLX directory), module
  selection, connect/disconnect, push-to-talk, last-received header, Last Heard
  list.
- **DMR tab:** searchable BrandMeister talkgroup picker, group and private
  (unit-to-unit) calls, current-subscription display, favourites list, Last
  Heard list with callsign and talkgroup-name lookup.
- **Settings:** callsign/DMR identity, audio input/output device with test
  buttons (tone for output, live level meter for input), ThumbDV serial device.
- Directory data (reflectors, talkgroups, DMR IDs) is cached on disk and
  refreshed at most every 24 hours.

## Requirements

- Linux (developed on Ubuntu; also used on a Raspberry Pi)
- A ThumbDV USB dongle
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
`--recurse-submodules`. The repo is currently private, so you need access to it
and a configured SSH key (or a GitHub token if you clone over HTTPS).

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

| Binary | Purpose |
| --- | --- |
| `dv3000client_gui` | The Qt GUI (D-Star and DMR tabs) |
| `dextra_test` | Command-line DExtra client, incl. live audio mode |
| `dmr_test` | Command-line DMR/Homebrew client |
| `roundtrip_test` | PCM to AMBE to PCM round trip through the ThumbDV |
| `xlx_directory_test` | Fetches and prints the XLX reflector directory |

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
latency timer causes audio glitches (playback xruns, growing backlog). Set it
to 1 ms permanently with a udev rule:

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

On Wi-Fi-connected machines (e.g. a Raspberry Pi), turning off Wi-Fi power
saving also helps keep audio smooth; see `HowToStart.md`.

## Running the GUI

```sh
./build/dv3000client_gui
```

1. Open **File > Settings...** and fill in:
   - **General:** your D-Star callsign and module letter; for DMR, your DMR
     ID, hotspot password and server (`host:port`), plus color code and time
     slot. Location, description and URL are optional and appear on the
     network's dashboard.
   - **Devices:** audio input and output devices (use the Test buttons to
     confirm you picked the right ones) and the ThumbDV serial device.
2. **D-Star tab:** pick a reflector, choose the target module, click
   **Connect**.
3. **DMR tab:** click **Connect**, pick or type a talkgroup, then use
   **PTT to send**.
4. **PTT to send** toggles transmit on and off (click it, or press the space
   bar when a text field isn't focused).

Settings can't be changed while a session is connected.

### DMR notes

- The DMR ID is your registered ID; the hotspot password is the one set in
  your BrandMeister account for hotspot/repeater use, not your account
  password.
- **DMR ID suffix:** to run this client alongside another one (e.g. BlueDV)
  under the same DMR ID, enter a 2-digit suffix. It's appended to form a
  unique 9-digit repeater ID. Leave it blank to use the plain 7-digit ID.
- BrandMeister has no persistent "connected talkgroup". You're subscribed to
  whichever talkgroup you last transmitted on, which is what the **Current
  subscription** line shows. Transmitting to TG 4000 unsubscribes; it's shown
  as "None" but can still be saved as a favourite.
- For a **private call** (e.g. the BrandMeister Parrot echo test, ID 9990),
  tick **Private call**; the talkgroup field is then treated as a target DMR
  ID.
- BrandMeister only accepts connections that identify as a recognised
  hotspot type. The Software ID / Package ID strings sent during login are
  copied from a working Pi-Star session; see the comments in
  `src/dmr_client.cpp`. If you plan to publish this, talk to BrandMeister
  support first.

### Files the GUI writes

Under `~/.config/DV3000Client/`:

| File | Contents |
| --- | --- |
| `settings.json` | Your settings (includes the DMR hotspot password in plain text) |
| `dmr_favourites.json` | DMR favourites list |
| `dv3000client.log` | Log file (also reachable via **Help > Log File Location...**) |
| `cache/` | Cached reflector, talkgroup and DMR ID directories |

## Command-line tools

Examples; run any tool with no arguments for its usage line.

```sh
# D-Star: link to a reflector on module B and go live with audio devices
./build/dextra_test <reflector-ip> B /dev/serial/by-id/<thumbdv> live plughw:1,0 plughw:1,0

# DMR: log in to a master and listen for 15 seconds
./build/dmr_test <host> <port> <dmrId> <password> [callsign] [seconds] [txTalkgroup]
```

`HowToStart.md` has more worked examples: recording received audio, converting
raw audio to WAV, and running on a Raspberry Pi.

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
src/gui/              Qt GUI (main window, D-Star tab, DMR tab, settings)
data/                 static fallback reflector list
scripts/              local xlxd test-reflector helper
third_party/serialDV  ThumbDV/AMBE3000 serial driver (fork, submodule)
third_party/xlxd      xlxd reference code (submodule; DMR FEC code is reused)
```

## Acknowledgements

- [serialDV](https://github.com/f4exb/serialDV) by F4EXB for the ThumbDV
  serial interface (this repo uses a fork)
- [xlxd](https://github.com/LX3JL/xlxd) by LX3JL, whose D-Star/DMR code
  informed the protocol implementations and whose FEC routines are reused
