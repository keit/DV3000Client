#pragma once

// BrandMeister's public talkgroup directory (api.brandmeister.network/v2/
// talkgroup -- a JSON object mapping talkgroup id to name, ~1800 entries
// covering worldwide/regional/country/local groups, e.g. "5302":"ZL2
// Regional"). GUI-only (unlike xlx_directory.h, nothing CLI-side needs
// this), so this parses with QJsonDocument rather than hand-rolling JSON
// parsing -- simpler and correctly handles the \uXXXX-escaped non-ASCII
// names (e.g. Greek, Cyrillic country names) the API returns.

#include <QString>
#include <cstdint>
#include <vector>

namespace bm {

struct TalkgroupInfo {
    uint32_t id = 0;
    QString name;
};

// Fetches the live directory over plain HTTP (the API happens to serve
// both HTTP and HTTPS; HTTP avoids needing a TLS-capable client for one
// small occasional GET -- see http_get.h). Returns false with `error` set
// on any network/parse failure.
bool fetchTalkgroupList(std::vector<TalkgroupInfo> &out, QString &error);

} // namespace bm
