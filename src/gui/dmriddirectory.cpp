#include "dmriddirectory.h"

#include <QStringList>

#include "http_get.h"
#include "localcache.h"

namespace dmr {

namespace {
constexpr const char *XLX_API_HOST = "xlxapi.rlx.lu";
constexpr const char *XLX_API_PATH = "/api/exportdmr.php";
constexpr int HTTP_PORT = 80;
constexpr const char *CACHE_KEY = "dmr_ids";

// "id;callsign;" per line -- a trailing empty field after the last ';' is
// expected and simply ignored by only looking at the first two.
bool parseDmrIdDirectory(const QByteArray &body, QHash<uint32_t, QString> &out) {
    out.clear();
    QString text = QString::fromUtf8(body);
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(';');
        if (fields.size() < 2) continue;
        bool ok = false;
        uint32_t id = fields[0].toUInt(&ok);
        if (!ok || fields[1].isEmpty()) continue;
        out.insert(id, fields[1]);
    }
    return !out.isEmpty();
}
} // namespace

bool fetchDmrIdDirectory(QHash<uint32_t, QString> &out, QString &error) {
    std::string body, err;
    if (!httpGetRaw(XLX_API_HOST, XLX_API_PATH, HTTP_PORT, body, err)) {
        error = QString::fromStdString(err);
        return false;
    }

    QByteArray bodyBytes(body.data(), static_cast<int>(body.size()));
    if (!parseDmrIdDirectory(bodyBytes, out)) {
        error = "no DMR IDs parsed from " + QString(XLX_API_HOST) + " response";
        return false;
    }
    cache::write(CACHE_KEY, bodyBytes);
    return true;
}

bool loadCachedDmrIdDirectory(QHash<uint32_t, QString> &out) {
    QByteArray body;
    if (!cache::read(CACHE_KEY, body)) return false;
    return parseDmrIdDirectory(body, out);
}

bool isDmrIdDirectoryCacheFresh(qint64 maxAgeSeconds) { return cache::isFresh(CACHE_KEY, maxAgeSeconds); }

} // namespace dmr
