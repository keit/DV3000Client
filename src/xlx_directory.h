// XLX/XRF reflector directory: fetches the live XLX reflector list
// (xlxapi.rlx.lu, the same "calling home" registry xlxd's own dashboard
// uses) and resolves user-facing reflector queries ("123", "XLX123",
// "XRF123") to a connectable host, falling back to a bundled static list
// for XRF reflectors that never migrated to xlxd (see data/DExtra_Hosts.txt).
//
// Rationale for treating XLX and XRF as one numbering space: an XLX
// reflector's DExtra module interface is what a classic XRF client already
// speaks, and the overwhelming majority of XRF-numbered reflectors that are
// still alive today are actually the same box, migrated to xlxd under its
// traditional number.
#pragma once

#include <string>
#include <vector>

namespace xlx {

struct ReflectorInfo {
    std::string name;         // e.g. "XLX123" or "XRF587"
    std::string host;         // IP or hostname; feed straight into DextraClient::open()
    std::string country;
    std::string comment;
    long lastContactUnix = 0; // 0 for static fallback entries (no live heartbeat)
};

// Fetches the live reflector directory over HTTP from xlxapi.rlx.lu.
// Returns false (with `error` set) on any network or parse failure.
bool fetchReflectorList(std::vector<ReflectorInfo> &out, std::string &error);

// Parses a legacy DExtra_Hosts.txt-style file ("NAME<whitespace>host" per
// line, blank lines and '#' comments ignored) as a fallback list for
// reflectors with no live entry. Returns false if the file can't be opened.
bool loadStaticFallback(const std::string &path, std::vector<ReflectorInfo> &out);

// Resolves a query -- "123", "XLX123", or "XRF123" (case-insensitive, any
// leading letters ignored) -- against `live` first, then `fallback`.
// Returns nullptr if `query` has no digits, or matches nothing in either
// list.
const ReflectorInfo *findReflector(const std::vector<ReflectorInfo> &live,
                                    const std::vector<ReflectorInfo> &fallback,
                                    const std::string &query);

} // namespace xlx
