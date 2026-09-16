#pragma once

// Flat list model over the live BrandMeister talkgroup directory (see
// talkgroupdirectory.h), with a small hardcoded fallback (the handful of
// universal/well-known entries verified directly against a live fetch --
// not a guess) so the list is never empty if the fetch fails. Mirrors
// ReflectorListModel's shape, but there's no "valid selection required"
// concept here the way there is for reflectors: unlike a reflector (which
// must be a real, resolvable host), any positive integer is a legal DMR
// talkgroup, listed or not -- see DmrTab, which reads whatever number the
// user has typed or picked rather than requiring a model row match.

#include <QAbstractListModel>

#include <vector>

#include "talkgroupdirectory.h"

class TalkgroupListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
    };

    explicit TalkgroupListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // Kicks off an async refresh of the live directory. Safe to call
    // repeatedly; a refresh already in flight is left to finish.
    void refresh();

signals:
    void refreshFailed(const QString &error);

private:
    void applyLive(std::vector<bm::TalkgroupInfo> live);

    std::vector<bm::TalkgroupInfo> m_rows;
    bool m_refreshing = false;
};
