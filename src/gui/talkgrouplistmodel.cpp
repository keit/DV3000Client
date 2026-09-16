#include "talkgrouplistmodel.h"

#include <algorithm>
#include <thread>

#include <QMetaObject>

namespace {

// Universal, well-known entries, verified directly against a live fetch of
// api.brandmeister.network/v2/talkgroup (not guessed) -- just enough to
// keep the search box useful if the live fetch fails (offline, DNS down,
// etc.), not an attempt at a full offline directory.
std::vector<bm::TalkgroupInfo> staticFallback() {
    return {
        {1, "Local"},   {2, "Cluster"}, {8, "Regional"},          {9, "Local"},
        {91, "World-wide"}, {92, "Europe"}, {93, "North America"},
        {94, "Asia, Middle East"}, {95, "Australia, New Zealand"}, {98, "Radio Test"},
    };
}

QString displayText(const bm::TalkgroupInfo &tg) {
    return QString::number(tg.id) + " — " + tg.name;
}

} // namespace

TalkgroupListModel::TalkgroupListModel(QObject *parent) : QAbstractListModel(parent) {
    m_rows = staticFallback();
}

int TalkgroupListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant TalkgroupListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) return {};

    const bm::TalkgroupInfo &tg = m_rows[static_cast<size_t>(index.row())];
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
    m_refreshing = true;

    std::thread([this] {
        std::vector<bm::TalkgroupInfo> live;
        QString error;
        bool ok = bm::fetchTalkgroupList(live, error);

        QMetaObject::invokeMethod(
            this,
            [this, ok, live = std::move(live), error]() mutable {
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

void TalkgroupListModel::applyLive(std::vector<bm::TalkgroupInfo> live) {
    beginResetModel();
    std::sort(live.begin(), live.end(), [](const bm::TalkgroupInfo &a, const bm::TalkgroupInfo &b) {
        return a.id < b.id;
    });
    m_rows = std::move(live);
    endResetModel();
}
