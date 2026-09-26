#include "masterdirectory.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

#include "http_get.h"
#include "localcache.h"

namespace bmmaster {

namespace {
constexpr const char *API_HOST = "api.brandmeister.network";
constexpr const char *API_PATH = "/v2/master";
constexpr int HTTP_PORT = 80;
constexpr const char *CACHE_KEY = "bm_masters";

bool parse(const QByteArray &body, std::vector<MasterInfo> &out, QString &error) {
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        error = QString("malformed response: ") + parseError.errorString();
        return false;
    }
    out.clear();
    for (const QJsonValue &v : doc.array()) {
        QJsonObject o = v.toObject();
        int id = o["id"].toInt();
        if (id <= 0) continue;
        out.push_back({static_cast<uint32_t>(id), o["country"].toString()});
    }
    std::sort(out.begin(), out.end(), [](const MasterInfo &a, const MasterInfo &b) {
        return a.country != b.country ? a.country < b.country : a.id < b.id;
    });
    return !out.empty();
}
} // namespace

bool fetchMasterList(std::vector<MasterInfo> &out, QString &error) {
    std::string body, err;
    if (!httpGetRaw(API_HOST, API_PATH, HTTP_PORT, body, err)) {
        error = QString::fromStdString(err);
        return false;
    }
    QByteArray bytes(body.data(), static_cast<int>(body.size()));
    if (!parse(bytes, out, error)) {
        if (error.isEmpty()) error = "no masters in the BrandMeister response";
        return false;
    }
    cache::write(CACHE_KEY, bytes);
    return true;
}

bool loadCachedMasterList(std::vector<MasterInfo> &out) {
    QByteArray body;
    if (!cache::read(CACHE_KEY, body)) return false;
    QString error; // a malformed cache is just treated as "no cache"
    return parse(body, out, error);
}

bool isMasterCacheFresh(qint64 maxAgeSeconds) { return cache::isFresh(CACHE_KEY, maxAgeSeconds); }

} // namespace bmmaster
