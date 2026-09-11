#include "reflectorlistmodel.h"

#include <algorithm>
#include <thread>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

namespace {

// data/DExtra_Hosts.txt is a repo-relative path everywhere else in this
// project too (see dextra_test.cpp), which assumes the binary is run from
// the repo root. Try that first, then fall back to paths relative to the
// binary itself so running straight from build/ also works.
QString findFallbackHostsFile() {
    QStringList candidates{
        "data/DExtra_Hosts.txt",
        QCoreApplication::applicationDirPath() + "/../data/DExtra_Hosts.txt",
        QCoreApplication::applicationDirPath() + "/data/DExtra_Hosts.txt",
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c)) return c;
    }
    return candidates.first();
}

QString displayText(const xlx::ReflectorInfo &r) {
    QString text = QString::fromStdString(r.name);
    if (!r.country.empty()) text += " — " + QString::fromStdString(r.country);
    if (!r.comment.empty()) text += " — " + QString::fromStdString(r.comment);
    return text;
}

} // namespace

ReflectorListModel::ReflectorListModel(QObject *parent) : QAbstractListModel(parent) {
    xlx::loadStaticFallback(findFallbackHostsFile().toStdString(), m_fallback);
    rebuildRows();
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
    m_refreshing = true;

    std::thread([this] {
        std::vector<xlx::ReflectorInfo> live;
        std::string error;
        bool ok = xlx::fetchReflectorList(live, error);

        QMetaObject::invokeMethod(
            this,
            [this, ok, live = std::move(live), error]() mutable {
                m_refreshing = false;
                if (ok) {
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
