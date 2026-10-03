#pragma once

// The ThumbDV's FTDI chip buffers serial data for up to its latency timer
// before handing it to the host -- 16 ms by default, which shows up as
// playback xruns and a growing backlog (see README.md's "FTDI latency
// timer"). The lasting fix is a udev rule, which needs root and is a
// manual step; this is the best the app can do on its own: ask the driver
// for 1 ms itself (ASYNC_LOW_LATENCY via TIOCSSERIAL, the same as
// `setserial <dev> low_latency`, which ftdi_sio lets an ordinary user with
// access to the port set), then report what the driver actually ended up
// with, so the GUI can tell the user when the udev rule is still needed.

#include <string>

namespace ftdi {

struct LatencyStatus {
    // False when the device has no latency timer to check: not a
    // USB-serial device at all (an AMBEServer host:port, a ttyACM, ...),
    // or one whose driver doesn't expose latency_timer.
    bool applicable = false;
    std::string ttyName; // e.g. "ttyUSB0", the name /sys knows it by
    int latencyMs = -1;  // as the driver reports it, after any attempt to lower it
    bool ok() const { return !applicable || latencyMs == 1; }
};

// device: the configured serial path, e.g. /dev/serial/by-id/... (symlinks
// are followed). Only touches the port if its latency timer isn't already
// 1 ms. Call while the port is already open elsewhere (e.g. right after
// DVController::open), so the brief extra open/close here isn't the last
// close and can't drop DTR/RTS on the device.
LatencyStatus ensureLowLatency(const std::string &device);

} // namespace ftdi
