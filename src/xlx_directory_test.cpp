// Standalone demo/test executable for the XLX/XRF reflector directory
// module (xlx_directory.h): fetches the live XLX reflector list, prints a
// summary, and -- if given an argument -- resolves it to a host via
// xlx::findReflector, the same lookup dextra_test uses to accept
// "XLX123"/"XRF123"/"123" in place of a raw IP.
//
// Usage: xlx_directory_test [query]

#include <cstdio>
#include <string>
#include <vector>

#include "xlx_directory.h"

int main(int argc, char **argv) {
    std::vector<xlx::ReflectorInfo> live;
    std::string error;
    if (!xlx::fetchReflectorList(live, error)) {
        std::fprintf(stderr, "xlx_directory_test: %s\n", error.c_str());
        return 1;
    }
    std::printf("Fetched %zu live reflectors from xlxapi.rlx.lu\n", live.size());
    for (size_t i = 0; i < live.size() && i < 5; i++) {
        const auto &r = live[i];
        std::printf("  %-8s %-16s %s\n", r.name.c_str(), r.host.c_str(), r.country.c_str());
    }

    std::vector<xlx::ReflectorInfo> fallback;
    xlx::loadStaticFallback("data/DExtra_Hosts.txt", fallback);
    std::printf("Loaded %zu static fallback entries\n", fallback.size());

    if (argc >= 2) {
        const std::string query = argv[1];
        const xlx::ReflectorInfo *found = xlx::findReflector(live, fallback, query);
        if (found) {
            std::printf("%s -> %s (%s)%s\n", query.c_str(), found->host.c_str(),
                        found->name.c_str(),
                        found->lastContactUnix == 0 ? " [static fallback]" : "");
        } else {
            std::printf("%s: not found\n", query.c_str());
            return 1;
        }
    }
    return 0;
}
