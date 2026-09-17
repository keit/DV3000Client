#include "talkgroupdirectory.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "http_get.h"
#include "localcache.h"

namespace bm {

namespace {
constexpr const char *BM_API_HOST = "api.brandmeister.network";
constexpr const char *BM_API_PATH = "/v2/talkgroup";
constexpr int HTTP_PORT = 80;
constexpr const char *CACHE_KEY = "talkgroups";

bool parseTalkgroupList(const QByteArray &body, std::vector<TalkgroupInfo> &out, QString &error) {
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QString("malformed response: ") + parseError.errorString();
        return false;
    }

    QJsonObject obj = doc.object();
    out.clear();
    out.reserve(static_cast<size_t>(obj.size()));
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool ok = false;
        uint32_t id = it.key().toUInt(&ok);
        if (!ok) continue;
        out.push_back({id, it.value().toString()});
    }
    return !out.empty();
}
} // namespace

bool fetchTalkgroupList(std::vector<TalkgroupInfo> &out, QString &error) {
    std::string body, err;
    if (!httpGetRaw(BM_API_HOST, BM_API_PATH, HTTP_PORT, body, err)) {
        error = QString::fromStdString(err);
        return false;
    }

    QByteArray bodyBytes(body.data(), static_cast<int>(body.size()));
    if (!parseTalkgroupList(bodyBytes, out, error)) {
        if (error.isEmpty()) error = "no talkgroups parsed from " + QString(BM_API_HOST) + " response";
        return false;
    }
    cache::write(CACHE_KEY, bodyBytes);
    return true;
}

bool loadCachedTalkgroupList(std::vector<TalkgroupInfo> &out) {
    QByteArray body;
    if (!cache::read(CACHE_KEY, body)) return false;
    QString error; // discarded -- a malformed cache is just treated as "no cache"
    return parseTalkgroupList(body, out, error);
}

bool isTalkgroupCacheFresh(qint64 maxAgeSeconds) { return cache::isFresh(CACHE_KEY, maxAgeSeconds); }

} // namespace bm
