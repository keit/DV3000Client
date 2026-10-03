#include "ftdi_latency.h"

#include <climits>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ftdi {

namespace {

int readLatencyTimer(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return -1;
    int value = -1;
    if (std::fscanf(f, "%d", &value) != 1) value = -1;
    std::fclose(f);
    return value;
}

// Best effort: any failure just leaves the timer where it was, which the
// caller finds out by reading it back.
void requestLowLatency(const std::string &device) {
    int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return;
    serial_struct ss{};
    // Only write back a struct the driver actually filled in.
    if (::ioctl(fd, TIOCGSERIAL, &ss) == 0) {
        ss.flags |= ASYNC_LOW_LATENCY;
        ::ioctl(fd, TIOCSSERIAL, &ss);
    }
    ::close(fd);
}

} // namespace

LatencyStatus ensureLowLatency(const std::string &device) {
    LatencyStatus status;
    char resolved[PATH_MAX];
    if (!::realpath(device.c_str(), resolved)) return status;
    std::string path(resolved);
    status.ttyName = path.substr(path.find_last_of('/') + 1);

    std::string sysfs = "/sys/bus/usb-serial/devices/" + status.ttyName + "/latency_timer";
    status.latencyMs = readLatencyTimer(sysfs);
    if (status.latencyMs < 0) return status;
    status.applicable = true;
    if (status.latencyMs == 1) return status;

    int before = status.latencyMs;
    requestLowLatency(path);
    status.latencyMs = readLatencyTimer(sysfs);
    if (status.latencyMs == 1) {
        std::fprintf(stderr, "ftdi: %s latency timer lowered from %d ms to 1 ms\n", status.ttyName.c_str(), before);
    } else {
        std::fprintf(stderr, "ftdi: %s latency timer is %d ms and could not be lowered to 1 ms -- install the udev rule\n",
                     status.ttyName.c_str(), status.latencyMs);
    }
    return status;
}

} // namespace ftdi
