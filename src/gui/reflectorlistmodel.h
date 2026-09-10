#pragma once

// Flat list model over the combined live XLX directory + static XRF
// fallback (see xlx_directory.h). The fallback loads synchronously at
// construction (it's a local file) so the list is never empty; refresh()
// fetches the live half in the background (it's a blocking HTTP call) and
// merges it in -- live entries take priority over a fallback entry with
// the same reflector number, since the live one has a real, current host.

#include <QAbstractListModel>

#include <vector>

#include "xlx_directory.h"

class ReflectorListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        HostRole = Qt::UserRole + 1,
        NameRole,
    };

    explicit ReflectorListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // Kicks off an async refresh of the live reflector list. Safe to call
    // repeatedly; a refresh already in flight is left to finish and a new
    // one isn't started on top of it.
    void refresh();

signals:
    void refreshFailed(const QString &error);

private:
    void applyLive(std::vector<xlx::ReflectorInfo> live);
    void rebuildRows();

    std::vector<xlx::ReflectorInfo> m_fallback;
    std::vector<xlx::ReflectorInfo> m_live;
    std::vector<const xlx::ReflectorInfo *> m_rows;
    bool m_refreshing = false;
};
