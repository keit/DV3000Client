#!/bin/bash
# Installs ambeserver-watchdog on a Pi-Star (or any systemd) box. Run it on
# the Pi from this directory:
#
#   sudo ./install-ambeserver-watchdog.sh
#
# Pi-Star keeps its root filesystem read-only, so this remounts it
# read-write for the copy and puts it back afterwards.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "run with sudo" >&2
    exit 1
fi

cd "$(dirname "$0")"

remounted=false
if findmnt -no OPTIONS / | grep -qw ro; then
    mount -o remount,rw /
    remounted=true
fi

install -m 755 ambeserver-watchdog.sh /usr/local/sbin/ambeserver-watchdog.sh
install -m 644 ambeserver-watchdog.service /etc/systemd/system/ambeserver-watchdog.service
systemctl daemon-reload
systemctl enable --now ambeserver-watchdog.service

if $remounted; then
    sync
    mount -o remount,ro / || echo "could not remount / read-only (busy?); run rpi-ro later" >&2
fi

systemctl --no-pager status ambeserver-watchdog.service | head -3
