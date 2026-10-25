#include "ysfreflectormodel.h"

#include <algorithm>
#include <thread>

#include <QMetaObject>
#include <QTimer>

#include "localcache.h"

namespace {
constexpr const char *CACHE_KEY = "ysf_hosts";
} // namespace

YsfReflectorModel::YsfReflectorModel(QObject *parent) : QAbstractListModel(parent) {
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

void YsfReflectorModel::setList(std::vector<ysf::ReflectorEntry> list) {
    std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    beginResetModel();
    m_rows = std::move(list);
    endResetModel();
}
