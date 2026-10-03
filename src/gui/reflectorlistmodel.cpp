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
    });
}

int ReflectorListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant ReflectorListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};

    const xlx::ReflectorInfo *r = m_rows[static_cast<size_t>(index.row())];
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

void ReflectorListModel::applyLive(std::vector<xlx::ReflectorInfo> live) {
    m_live = std::move(live);
    rebuildRows();
}

void ReflectorListModel::rebuildRows() {
    beginResetModel();
    m_rows.clear();
    m_rows.reserve(m_live.size() + m_fallback.size());
    for (const auto &r : m_live) m_rows.push_back(&r);
    for (const auto &r : m_fallback) m_rows.push_back(&r);
    std::sort(m_rows.begin(), m_rows.end(),
              [](const xlx::ReflectorInfo *a, const xlx::ReflectorInfo *b) { return a->name < b->name; });
    endResetModel();
}
