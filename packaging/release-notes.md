## Install

**With apt** (recommended; updates arrive with your system updates). Add the
[keit APT repository](https://keit.github.io/apt/) once, then `sudo apt install dv3kclient`.
If you already use it, `sudo apt update && sudo apt upgrade` gets this release.

**Or download** a file below:

| Computer | .deb | AppImage |
|---|---|---|
| PC (x86_64): Debian 12, Ubuntu 24.04 or newer | `dv3kclient_*_amd64.deb` | `DV3KClient-x86_64.AppImage` |
| Raspberry Pi 3 or later: 64-bit Raspberry Pi OS with desktop, Bookworm or newer | `dv3kclient_*_arm64.deb` | `DV3KClient-aarch64.AppImage` |

**.deb:**

```sh
sudo apt install ./dv3kclient_*.deb
```

**AppImage** (also runs on most other Linux distributions):

```sh
chmod +x DV3KClient-*.AppImage
./DV3KClient-*.AppImage
```

Before the first connect, follow [Getting Started](https://keit.github.io/DV3000Client/getting-started.html): serial port access for a ThumbDV plugged into the computer running DV3K Client, or the AMBEServer setup if it's on a Raspberry Pi of its own.

`SHA256SUMS` lists the checksums of every file below; `dv3kclient-*.tar.gz` is the complete source, submodules included.
