#!/bin/bash
# Restarts AMBEServer when the ThumbDV's USB serial link stalls.
#
# On a Raspberry Pi 3B+ the ThumbDV's FTDI chip occasionally stalls its
# USB bulk-in endpoint, and the kernel logs:
#
#   ftdi_sio ttyUSB0: usb_serial_generic_read_bulk_callback - urb stopped: -32
#
# After that the port stops delivering data until it is closed and
# reopened. AMBEServer keeps the port open forever, so it goes deaf: every
# request from a client times out until someone restarts it (seen lasting
# all night). This follows the kernel log and restarts the service when
# the message appears. It never talks to AMBEServer itself, so it can't
# disturb a connected client.
#
# Installed by install-ambeserver-watchdog.sh; runs as a systemd service.

SERVICE=${AMBESERVER_SERVICE:-ambeserver}
# The stall message can come in quick bursts; one restart covers them all.
QUIET_SECONDS=10

last_restart=0

# -n 0: only messages from now on, not the backlog since boot.
journalctl -k -f -n 0 -o cat | while read -r line; do
    case "$line" in
        *ftdi_sio*"urb stopped"*)
            now=$(date +%s)
            if (( now - last_restart < QUIET_SECONDS )); then
                continue
            fi
            last_restart=$now
            echo "USB stall on the ThumbDV (\"$line\"), restarting $SERVICE"
            sleep 1
            if systemctl restart "$SERVICE"; then
                echo "$SERVICE restarted"
            else
                echo "failed to restart $SERVICE"
            fi
            ;;
    esac
done
