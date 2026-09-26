#pragma once

// BrandMeister's list of master servers (api.brandmeister.network/v2/master:
// a JSON array of {id, country, address}, ~40 entries), for the Settings
// dialog's server dropdown -- shown as e.g. "AU 5051", the way BlueDV does.
// Each master is reachable at "<id>.master.brandmeister.network" (checked:
// every entry's hostname resolves to the address the API lists), so the
// hostname is what gets stored, not the IP, which BrandMeister may change.
// Same shape as talkgroupdirectory.h: cached on disk, re-fetched when stale.

#include <QString>
#include <cstdint>
#include <vector>

namespace bmmaster {

struct MasterInfo {
    uint32_t id = 0;
    QString country; // ISO code, e.g. "AU"

    QString host() const { return QString::number(id) + ".master.brandmeister.network"; }
    QString label() const { return country + " " + QString::number(id); } // "AU 5051"
};

// Fetches the live list (blocking -- call from a worker thread) and caches
// it on success. Sorted by country, then id. Returns false with `error` set
// on any network/parse failure.
bool fetchMasterList(std::vector<MasterInfo> &out, QString &error);

// The last cached list, if any -- synchronous and network-free.
bool loadCachedMasterList(std::vector<MasterInfo> &out);

bool isMasterCacheFresh(qint64 maxAgeSeconds);

} // namespace bmmaster
