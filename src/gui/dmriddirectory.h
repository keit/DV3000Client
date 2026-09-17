#pragma once

// DMR ID -> callsign lookup, resolved the same way MMDVMHost/Pi-Star and
// xlxd itself do it -- there's no callsign anywhere in the Homebrew wire
// protocol (DMRD's srcId/dstId are purely numeric), so a receiving client
// has no way to show "who's transmitting" as a callsign without a
// separately-fetched public directory. xlxapi.rlx.lu/api/exportdmr.php is
// the exact same source xlxd downloads at startup for its own DMR ID
// resolution (see the project's protocol-roadmap notes) -- a plain-text
// "id;callsign;" line per entry, ~300k entries.

#include <QHash>
#include <QString>
#include <cstdint>

namespace dmr {

// Fetches the full directory and returns it as an id->callsign map. On
// success, also writes the raw response to the local disk cache (see
// localcache.h) for loadCachedDmrIdDirectory() to use on a future
// startup. Returns false with `error` set on any network/parse failure;
// on success, `out` is never empty (a suspiciously small or unparseable
// response is treated as a failure, same reasoning as xlx_directory's
// own checks).
bool fetchDmrIdDirectory(QHash<uint32_t, QString> &out, QString &error);

// Loads the last successfully cached directory from disk, if any --
// synchronous and network-free, meant for populating the lookup
// immediately at startup while fetchDmrIdDirectory() refreshes it (and
// the cache) in the background. Returns false if there's no cache yet.
bool loadCachedDmrIdDirectory(QHash<uint32_t, QString> &out);

// True if the cached directory exists and is less than maxAgeSeconds old
// -- callers use this to skip fetchDmrIdDirectory() entirely on a given
// startup rather than re-fetching this ~330k-line directory every time.
bool isDmrIdDirectoryCacheFresh(qint64 maxAgeSeconds);

} // namespace dmr
