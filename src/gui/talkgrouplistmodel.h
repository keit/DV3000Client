#pragma once

// Flat list model over the live talkgroup directory of the selected DMR
// network (BrandMeister or TGIF -- see
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

    // Switches to `network`'s directory ("brandmeister" or "tgif"): its
    // cached copy (or a small built-in list when there's none yet) shows
    // immediately, and refresh() then brings in the live one. Talkgroup
    // numbers mean different things on different networks, so the two
    // lists are never mixed.
    void setNetwork(const QString &network);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // Kicks off an async refresh of the live directory. Safe to call
    // repeatedly; a refresh already in flight is left to finish.
    void refresh();

    // The directory's name for `id`, or an empty string if it's not
    // (yet) listed -- for resolving a heard talkgroup to a name outside
    // the combo box itself, e.g. DmrTab's last-heard log.
    QString nameForId(uint32_t id) const;

signals:
    void refreshFailed(const QString &error);

private:
    void applyLive(std::vector<tgdir::TalkgroupInfo> live);

    QString m_network = "brandmeister";
    std::vector<tgdir::TalkgroupInfo> m_rows;
    bool m_refreshing = false;
};
