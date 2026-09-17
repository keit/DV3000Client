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

// Fetches the full directory and returns it as an id->callsign map.
// Returns false with `error` set on any network/parse failure; on success,
// `out` is never empty (a suspiciously small or unparseable response is
// treated as a failure, same reasoning as xlx_directory's own checks).
bool fetchDmrIdDirectory(QHash<uint32_t, QString> &out, QString &error);

} // namespace dmr
