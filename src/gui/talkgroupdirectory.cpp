#include "talkgroupdirectory.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "http_get.h"
#include "localcache.h"

namespace tgdir {

namespace {
constexpr const char *BM_API_HOST = "api.brandmeister.network";
constexpr const char *BM_API_PATH = "/v2/talkgroup";
constexpr int HTTP_PORT = 80;
constexpr const char *TGIF_API_URL = "https://api.tgif.network/dmr/talkgroups/json";

// "talkgroups" is BrandMeister's original cache name, kept so an existing
// cache carries over.
const char *cacheKey(const QString &network) { return network == "tgif" ? "talkgroups_tgif" : "talkgroups"; }

bool parseBrandmeister(const QByteArray &body, std::vector<TalkgroupInfo> &out, QString &error) {
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

bool parseTgif(const QByteArray &body, std::vector<TalkgroupInfo> &out, QString &error) {
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        error = QString("malformed response: ") + parseError.errorString();
        return false;
    }

    QJsonArray array = doc.array();
    out.clear();
    out.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue &v : array) {
        QJsonObject o = v.toObject();
        bool ok = false;
        uint32_t id = o["id"].toString().toUInt(&ok); // the id arrives as a string
        if (!ok) continue;
        out.push_back({id, o["name"].toString()});
    }
    return !out.empty();
}

bool parse(const QString &network, const QByteArray &body, std::vector<TalkgroupInfo> &out, QString &error) {
    return network == "tgif" ? parseTgif(body, out, error) : parseBrandmeister(body, out, error);
}

bool fetchBody(const QString &network, QByteArray &body, QString &error) {
    if (network != "tgif") {
        std::string raw, err;
        if (!httpGetRaw(BM_API_HOST, BM_API_PATH, HTTP_PORT, raw, err)) {
            error = QString::fromStdString(err);
            return false;
        }
        body = QByteArray(raw.data(), static_cast<int>(raw.size()));
        return true;
    }

    // Runs on a plain std::thread with no event loop of its own, so drive
    // one locally until the reply finishes.
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(TGIF_API_URL)};
    request.setTransferTimeout(30000);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    bool ok = reply->error() == QNetworkReply::NoError;
    if (ok) body = reply->readAll();
    else error = reply->errorString();
    reply->deleteLater();
    return ok;
}
} // namespace

bool fetchTalkgroupList(const QString &network, std::vector<TalkgroupInfo> &out, QString &error) {
    QByteArray body;
    if (!fetchBody(network, body, error)) return false;
    if (!parse(network, body, out, error)) {
        if (error.isEmpty()) error = "no talkgroups parsed from the " + network + " directory response";
        return false;
    }
    cache::write(cacheKey(network), body);
    return true;
}

bool loadCachedTalkgroupList(const QString &network, std::vector<TalkgroupInfo> &out) {
    QByteArray body;
    if (!cache::read(cacheKey(network), body)) return false;
    QString error; // discarded -- a malformed cache is just treated as "no cache"
    return parse(network, body, out, error);
}

bool isTalkgroupCacheFresh(const QString &network, qint64 maxAgeSeconds) {
    return cache::isFresh(cacheKey(network), maxAgeSeconds);
}

} // namespace tgdir
