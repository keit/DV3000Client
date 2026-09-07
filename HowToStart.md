## Build

cmake --build build

## Find ThumbDV device id

ls -la /dev/serial/by-id

## Run XLXd on Ubuntu

./scripts/run-test-xlxd.sh XLX999 0.0.0.0

## Run dextra_test on Ubuntu

./build/dextra_test 127.0.0.1 B /dev/serial/by-id/usb-NW_Digital_Radio_09_25_ThumbDV_D30G0J77-if00-port0 live plughw:1,0 plughw:1,0

## Run dextra_test on Raspberry Pi with recording

./build/dextra_test 192.168.68.58 B /dev/serial/by-id/usb-NW_Digital_Radio_09_25_ThumbDV_D30G37BE-if00-port0 live null plughw:0,0 /tmp/rx_capture.raw

## Play back the recorded audio

aplay -f S16_LE -r 8000 -c 1 /tmp/rx_capture.raw

## Concert raw format to wav format

sox -t raw -r 8000 -e signed -b 16 -c 1 /tmp/rx_capture.raw /tmp/rx_capture.wav

## Latency timer

cat /sys/bus/usb-serial/devices/ttyUSB0/latency_timer
