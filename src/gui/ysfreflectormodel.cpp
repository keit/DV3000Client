#include "ysfreflectormodel.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#include <QFile>
#include <QMetaObject>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

#include "localcache.h"

namespace {
constexpr const char *CACHE_KEY = "ysf_hosts";
} // namespace

YsfReflectorModel::YsfReflectorModel(QObject *parent) : QAbstractListModel(parent) {
    QFile local(localFilePath());
    if (local.open(QIODevice::ReadOnly)) {
        m_local = ysf::parseReflectorList(local.readAll().toStdString());
        for (auto &r : m_local)
            r.description += r.description.empty() ? "(ysf_reflectors.txt)" : " (ysf_reflectors.txt)";
        std::fprintf(stderr, "ysf reflectors: %zu from %s\n", m_local.size(), localFilePath().toLocal8Bit().constData());
    }
    // Shown even before the directory: it may never have been fetched.
    if (!m_local.empty()) setList({});
    // Deferred a tick, like the D-Star list (see ReflectorListModel): a
    // large model before the window's first show upsets its sizing.
    QTimer::singleShot(0, this, [this] {
        QByteArray cached;
        if (cache::read(CACHE_KEY, cached)) setList(ysf::parseReflectorList(cached.toStdString()));
    });
}

int YsfReflectorModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant YsfReflectorModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) return {};
    const ysf::ReflectorEntry &r = m_rows[static_cast<size_t>(index.row())];
    // EditRole too: an editable combo box shows the picked row's EditRole.
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        QString text = QString::fromStdString(r.name);
        if (!r.description.empty() && r.description != r.name) text += " — " + QString::fromStdString(r.description);
        return text + " (" + QString::fromStdString(r.id) + ")";
    }
    if (role == HostRole) return QString::fromStdString(r.host);
    if (role == PortRole) return r.port;
    if (role == NameRole) return QString::fromStdString(r.name);
    return {};
}

void YsfReflectorModel::refresh() {
    if (m_refreshing || cache::isFresh(CACHE_KEY, cache::ONE_DAY_SECONDS)) return;
    m_refreshing = true;
    std::thread([this] {
        std::vector<ysf::ReflectorEntry> list;
        std::string error;
        bool ok = ysf::fetchReflectorList(list, error);
        QMetaObject::invokeMethod(
            this,
            [this, ok, list = std::move(list), error]() mutable {
                m_refreshing = false;
                if (!ok) {
                    emit refreshFailed(QString::fromStdString(error));
                    return;
                }
                // Cached in the list's own text format, for parseReflectorList.
                std::string text;
                for (const auto &r : list)
                    text += r.id + ";" + r.name + ";" + r.description + ";" + r.host + ";" + std::to_string(r.port) + ";\n";
                cache::write(CACHE_KEY, QByteArray::fromStdString(text));
                setList(std::move(list));
            },
            Qt::QueuedConnection);
    }).detach();
}

QString YsfReflectorModel::localFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/ysf_reflectors.txt";
}

void YsfReflectorModel::setList(std::vector<ysf::ReflectorEntry> list) {
    // ysf_reflectors.txt's entries replace the directory's of the same name.
    QSet<QString> overridden;
    for (const auto &r : m_local) overridden.insert(QString::fromStdString(r.name).toUpper());
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const auto &r) { return overridden.contains(QString::fromStdString(r.name).toUpper()); }),
               list.end());
    list.insert(list.end(), m_local.begin(), m_local.end());
    std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    beginResetModel();
    m_rows = std::move(list);
    endResetModel();
}
