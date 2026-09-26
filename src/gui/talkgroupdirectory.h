#pragma once

// Public talkgroup directories, one per DMR network -- each a JSON list of
// talkgroup id -> name, fetched live, cached on disk (see localcache.h) and
// re-read from that cache on the next startup:
//
//  - "brandmeister": api.brandmeister.network/v2/talkgroup, a JSON object
//    mapping id to name (~1800 entries, e.g. "5302":"ZL2 Regional"). Served
//    over plain HTTP too, so it uses the project's small raw-socket GET
//    (http_get.h) rather than needing a TLS client.
//  - "tgif": api.tgif.network/dmr/talkgroups/json, a JSON array of
//    {id (a string), name, website, description (base64)} objects (~3200
//    entries, ~1.4MB mostly from the descriptions, which are ignored).
//    HTTP just redirects to HTTPS there, so this one goes through Qt's
//    network stack.
//
// GUI-only (unlike xlx_directory.h, nothing CLI-side needs this), so both
// parse with QJsonDocument -- which also correctly handles the
// \uXXXX-escaped non-ASCII names (Greek, Cyrillic, ...) the APIs return.

#include <QString>
#include <cstdint>
#include <vector>

namespace tgdir {

struct TalkgroupInfo {
    uint32_t id = 0;
    QString name;
};

// Fetches the live directory for `network` ("brandmeister" or "tgif") and,
// on success, writes the raw response to the disk cache for
// loadCachedTalkgroupList() to use on a future startup. Blocks until done
// (call it from a worker thread). Returns false with `error` set on any
// network/parse failure.
bool fetchTalkgroupList(const QString &network, std::vector<TalkgroupInfo> &out, QString &error);

// Loads the last successfully cached directory for `network` from disk, if
// any -- synchronous and network-free. Returns false if there's no cache yet.
bool loadCachedTalkgroupList(const QString &network, std::vector<TalkgroupInfo> &out);

// True if that network's cached directory exists and is less than
// maxAgeSeconds old -- callers use this to skip fetching on a given startup.
bool isTalkgroupCacheFresh(const QString &network, qint64 maxAgeSeconds);

} // namespace tgdir
