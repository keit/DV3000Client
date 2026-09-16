#include "talkgroupdirectory.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "http_get.h"

namespace bm {

namespace {
constexpr const char *BM_API_HOST = "api.brandmeister.network";
constexpr const char *BM_API_PATH = "/v2/talkgroup";
constexpr int HTTP_PORT = 80;
} // namespace

bool fetchTalkgroupList(std::vector<TalkgroupInfo> &out, QString &error) {
    std::string body, err;
    if (!httpGetRaw(BM_API_HOST, BM_API_PATH, HTTP_PORT, body, err)) {
        error = QString::fromStdString(err);
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        error = "malformed response from " + QString(BM_API_HOST) + ": " + parseError.errorString();
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

    if (out.empty()) {
        error = "no talkgroups parsed from " + QString(BM_API_HOST) + " response";
        return false;
    }
    return true;
}

} // namespace bm
