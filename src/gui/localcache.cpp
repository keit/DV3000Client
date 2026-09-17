#include "localcache.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace cache {

namespace {
QString cacheFilePath(const QString &key) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/cache";
    QDir().mkpath(dir);
    return dir + "/" + key + ".cache";
}
} // namespace

bool read(const QString &key, QByteArray &data) {
    QFile f(cacheFilePath(key));
    if (!f.open(QIODevice::ReadOnly)) return false;
    data = f.readAll();
    return !data.isEmpty();
}

void write(const QString &key, const QByteArray &data) {
    QFile f(cacheFilePath(key));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(data);
}

bool isFresh(const QString &key, qint64 maxAgeSeconds) {
    QFileInfo info(cacheFilePath(key));
    if (!info.exists() || info.size() == 0) return false;
    return info.lastModified().secsTo(QDateTime::currentDateTime()) < maxAgeSeconds;
}

} // namespace cache
