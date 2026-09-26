#include "talkgrouplistmodel.h"

#include <algorithm>
#include <thread>

#include <QMetaObject>

#include "localcache.h"

namespace {

// Universal, well-known entries -- just enough to keep the search box
// useful before the first live fetch lands or if it fails (offline, DNS
// down, etc.), not an attempt at a full offline directory. BrandMeister's
// were verified directly against a live fetch of its API; TGIF's come from
// its FAQ (Parrot echo test on 9990 or 31000, main talkgroup 31665) and
// the live list, which doesn't include the two Parrot numbers.
std::vector<tgdir::TalkgroupInfo> staticFallback(const QString &network) {
    if (network == "tgif") {
        return {
            {9990, "Parrot (echo test)"},
            {31000, "Parrot (echo test)"},
            {31665, "TGIF The Mothership"},
        };
    }
    return {
        {1, "Local"},   {2, "Cluster"}, {8, "Regional"},          {9, "Local"},
        {91, "World-wide"}, {92, "Europe"}, {93, "North America"},
        {94, "Asia, Middle East"}, {95, "Australia, New Zealand"}, {98, "Radio Test"},
    };
}

QString displayText(const tgdir::TalkgroupInfo &tg) {
    return QString::number(tg.id) + " — " + tg.name;
}

} // namespace

TalkgroupListModel::TalkgroupListModel(QObject *parent) : QAbstractListModel(parent) {
    m_rows = staticFallback(m_network);
}

void TalkgroupListModel::setNetwork(const QString &network) {
    beginResetModel();
    m_network = network;
    // Last run's cached copy (if any) is a much better starting point than
    // the tiny hardcoded fallback -- refresh() still kicks off a live fetch
    // to replace this with current data in the background.
    if (tgdir::loadCachedTalkgroupList(m_network, m_rows)) {
        std::sort(m_rows.begin(), m_rows.end(),
                  [](const tgdir::TalkgroupInfo &a, const tgdir::TalkgroupInfo &b) { return a.id < b.id; });
    } else {
        m_rows = staticFallback(m_network);
    }
    // A fetch still in flight for the previous network must not block this
    // one's (its result is dropped on arrival -- see refresh()).
    m_refreshing = false;
    endResetModel();
}

int TalkgroupListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant TalkgroupListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) return {};

    const tgdir::TalkgroupInfo &tg = m_rows[static_cast<size_t>(index.row())];
    // EditRole matters even though nothing here is actually edited --
    // QComboBox::itemText() (used to fill an editable combo's line edit
    // when a row is picked via the dropdown arrow, as opposed to via a
    // QCompleter popup) reads EditRole, not DisplayRole. See
    // ReflectorListModel's identical fix for why this was worth a comment.
    if (role == Qt::DisplayRole || role == Qt::EditRole) return displayText(tg);
    if (role == IdRole) return tg.id;
    return {};
}

void TalkgroupListModel::refresh() {
    if (m_refreshing) return;
    // setNetwork() already loaded this same cached copy synchronously -- if
    // it's still fresh, m_rows already reflects it and there's nothing this
    // fetch would change.
    if (tgdir::isTalkgroupCacheFresh(m_network, cache::ONE_DAY_SECONDS)) return;
    m_refreshing = true;

    std::thread([this, network = m_network] {
        std::vector<tgdir::TalkgroupInfo> live;
        QString error;
        bool ok = tgdir::fetchTalkgroupList(network, live, error);

        QMetaObject::invokeMethod(
            this,
            [this, network, ok, live = std::move(live), error]() mutable {
                if (network != m_network) return; // the user has since switched networks
                m_refreshing = false;
                if (ok) {
                    applyLive(std::move(live));
                } else {
                    emit refreshFailed(error);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

QString TalkgroupListModel::nameForId(uint32_t id) const {
    for (const tgdir::TalkgroupInfo &tg : m_rows) {
        if (tg.id == id) return tg.name;
    }
    return {};
}

void TalkgroupListModel::applyLive(std::vector<tgdir::TalkgroupInfo> live) {
    beginResetModel();
    std::sort(live.begin(), live.end(), [](const tgdir::TalkgroupInfo &a, const tgdir::TalkgroupInfo &b) {
        return a.id < b.id;
    });
    m_rows = std::move(live);
    endResetModel();
}
