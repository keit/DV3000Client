#include "dmr_transport.h"

namespace dmr {

volatile sig_atomic_t g_running = 1;

const char *ToString(LinkResult result) {
    switch (result) {
    case LinkResult::Success: return "success";
    case LinkResult::LoginRejected: return "login rejected (DMR ID not recognised/permitted)";
    case LinkResult::AuthRejected: return "authentication rejected (wrong password)";
    case LinkResult::ConfigRejected: return "configuration rejected";
    case LinkResult::Timeout: return "timed out";
    }
    return "unknown";
}

} // namespace dmr
