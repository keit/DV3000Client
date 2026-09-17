#include "dmriddirectory.h"

#include <QStringList>

#include "http_get.h"

namespace dmr {

namespace {
constexpr const char *XLX_API_HOST = "xlxapi.rlx.lu";
constexpr const char *XLX_API_PATH = "/api/exportdmr.php";
constexpr int HTTP_PORT = 80;
} // namespace

bool fetchDmrIdDirectory(QHash<uint32_t, QString> &out, QString &error) {
    std::string body, err;
    if (!httpGetRaw(XLX_API_HOST, XLX_API_PATH, HTTP_PORT, body, err)) {
        error = QString::fromStdString(err);
        return false;
    }

    out.clear();
    QString text = QString::fromUtf8(body.data(), static_cast<int>(body.size()));
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // "id;callsign;" -- a trailing empty field after the last ';' is
        // expected and simply ignored by only looking at the first two.
        const QStringList fields = line.split(';');
        if (fields.size() < 2) continue;
        bool ok = false;
        uint32_t id = fields[0].toUInt(&ok);
        if (!ok || fields[1].isEmpty()) continue;
        out.insert(id, fields[1]);
    }

    if (out.isEmpty()) {
        error = "no DMR IDs parsed from " + QString(XLX_API_HOST) + " response";
        return false;
    }
    return true;
}

} // namespace dmr
