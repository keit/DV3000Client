./scripts/run-test-xlxd.sh

./build/dextra_test 127.0.0.1 B /dev/serial/by-id/usb-NW_Digital_Radio_09_25_ThumbDV_D30G37BE-if00-port0 live plughw:1,0 plughw:1,0

./build/dextra_test 127.0.0.1 B /dev/serial/by-id/usb-NW_Digital_Radio_09_25_ThumbDV_D30G0J77-if00-port0 live default default

ls -la /dev/serial/by-id
