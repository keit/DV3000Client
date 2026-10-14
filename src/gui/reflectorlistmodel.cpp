#include "reflectorlistmodel.h"

#include <algorithm>
#include <sstream>
#include <thread>

#include <QFile>
#include <QMetaObject>
#include <QTimer>

#include "localcache.h"

namespace {

constexpr const char *CACHE_KEY = "reflectors";
// One "name address" line per entry.
constexpr const char *DPLUS_CACHE_KEY = "dplus_hosts";

std::vector<xlx::ReflectorInfo> toReflectorInfo(const std::vector<dplus::GatewayEntry> &list) {
    std::vector<xlx::ReflectorInfo> out;
    for (const auto &g : list) {
        xlx::ReflectorInfo r;
        r.name = g.name;
        r.host = g.address;
        r.comment = g.name.rfind("REF", 0) == 0 ? "DPlus reflector" : "DPlus gateway";
        out.push_back(r);
    }
    return out;
}

QByteArray toCache(const std::vector<dplus::GatewayEntry> &list) {
    QByteArray data;
    for (const auto &g : list) data += QByteArray::fromStdString(g.name + " " + g.address + "\n");
    return data;
}

std::vector<dplus::GatewayEntry> fromCache(const QByteArray &data) {
    std::vector<dplus::GatewayEntry> list;
    for (const QByteArray &line : data.split('\n')) {
        QList<QByteArray> fields = line.split(' ');
        if (fields.size() == 2) list.push_back({fields[0].toStdString(), fields[1].toStdString()});
    }
    return list;
}

QString displayText(const xlx::ReflectorInfo &r) {
    QString text = QString::fromStdString(r.name);
    if (!r.country.empty()) text += " — " + QString::fromStdString(r.country);
    if (!r.comment.empty()) text += " — " + QString::fromStdString(r.comment);
    return text;
}

} // namespace

ReflectorListModel::ReflectorListModel(QObject *parent) : QAbstractListModel(parent) {
    // data/DExtra_Hosts.txt, built in via resources/dv3kclient.qrc so an
    // installed binary (deb, AppImage) has it wherever it's run from.
    QFile hosts(":/data/DExtra_Hosts.txt");
    if (hosts.open(QIODevice::ReadOnly)) {
        std::istringstream in(hosts.readAll().toStdString());
        xlx::loadStaticFallback(in, m_fallback);
    }
    rebuildRows();

    // Loading the cached live list is deferred by one event-loop tick
    // (rather than done here, synchronously, before the window is ever
    // shown) -- empirically, populating this model with its full live
    // row count *before* first show breaks window resize/splitter-drag
    // under GNOME/Wayland (confirmed by bisection; root cause is likely
    // the reflector combo's QCompleter popup -- a separate top-level
    // surface -- getting sized against a large model during the same
    // window's initial show/map handshake). A zero-delay QTimer still
    // fires effectively immediately from the user's perspective, just
    // after that handshake has settled instead of during it.
    QTimer::singleShot(0, this, [this] {
        QByteArray cached;
        if (cache::read(CACHE_KEY, cached)) {
            m_live = xlx::parseReflectorList(std::string(cached.constData(), static_cast<size_t>(cached.size())));
            rebuildRows();
        }
        if (cache::read(DPLUS_CACHE_KEY, cached)) {
            m_dplus = toReflectorInfo(fromCache(cached));
            rebuildRows();
        }
    });
}

int ReflectorListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant ReflectorListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};

    const xlx::ReflectorInfo *r = m_rows[static_cast<size_t>(index.row())].info;
    // EditRole matters here even though nothing in this model is actually
    // edited: QComboBox::itemText() (used to fill an editable combo's line
    // edit when a row is picked from its own dropdown, as opposed to from
    // a QCompleter popup, which instead uses completionRole -- DisplayRole
    // here) reads EditRole, not DisplayRole. Leaving it unhandled meant
    // picking a reflector via the dropdown arrow left the line edit
    // blank instead of showing what got picked.
    if (role == Qt::DisplayRole || role == Qt::EditRole) return displayText(*r);
    if (role == HostRole) return QString::fromStdString(r->host);
    if (role == NameRole) return QString::fromStdString(r->name);
    if (role == IsDPlusRole) return m_rows[static_cast<size_t>(index.row())].dplus;
    return {};
}

void ReflectorListModel::refresh() {
    if (m_refreshing) return;
    // The constructor's deferred QTimer::singleShot (above) already loads
    // this same cached copy -- if it's still fresh, there's nothing this
    // live fetch would change, whether or not that deferred load has
    // actually run yet (it reads the same on-disk file either way).
    if (cache::isFresh(CACHE_KEY, cache::ONE_DAY_SECONDS)) return;
    m_refreshing = true;

    std::thread([this] {
        std::string body, error;
        bool ok = xlx::fetchReflectorListRaw(body, error);
        std::vector<xlx::ReflectorInfo> live;
        if (ok) {
            live = xlx::parseReflectorList(body);
            if (live.empty()) {
                ok = false;
                error = "no reflectors parsed from response";
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, live = std::move(live), body = std::move(body), error]() mutable {
                m_refreshing = false;
                if (ok) {
                    cache::write(CACHE_KEY, QByteArray(body.data(), static_cast<int>(body.size())));
                    applyLive(std::move(live));
                } else {
                    emit refreshFailed(QString::fromStdString(error));
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void ReflectorListModel::refreshDPlus(const QString &callsign) {
    if (m_refreshingDPlus || callsign.isEmpty() || cache::isFresh(DPLUS_CACHE_KEY, cache::ONE_DAY_SECONDS)) return;
    m_refreshingDPlus = true;
    std::thread([this, callsign = callsign.toStdString()] {
        std::vector<dplus::GatewayEntry> list;
        std::string error;
        bool ok = dplus::authenticate(callsign, &list, error) && !list.empty();
        QMetaObject::invokeMethod(
            this,
            [this, ok, list = std::move(list), error]() mutable {
                m_refreshingDPlus = false;
                if (ok) {
                    setDPlusList(std::move(list));
                } else {
                    emit refreshFailed(QString::fromStdString("DPlus: " + error));
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void ReflectorListModel::setDPlusList(std::vector<dplus::GatewayEntry> list) {
    if (list.empty()) return;
    cache::write(DPLUS_CACHE_KEY, toCache(list));
    m_dplus = toReflectorInfo(list);
    rebuildRows();
}

void ReflectorListModel::applyLive(std::vector<xlx::ReflectorInfo> live) {
    m_live = std::move(live);
    rebuildRows();
}

void ReflectorListModel::rebuildRows() {
    beginResetModel();
    m_rows.clear();
    m_rows.reserve(m_live.size() + m_fallback.size() + m_dplus.size());
    for (const auto &r : m_live) m_rows.push_back({&r, false});
    for (const auto &r : m_fallback) m_rows.push_back({&r, false});
    for (const auto &r : m_dplus) m_rows.push_back({&r, true});
    std::sort(m_rows.begin(), m_rows.end(), [](const Row &a, const Row &b) { return a.info->name < b.info->name; });
    endResetModel();
}
