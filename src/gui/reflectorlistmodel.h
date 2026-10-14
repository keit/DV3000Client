#pragma once

// Flat list model over the combined live XLX directory + static XRF
// fallback (see xlx_directory.h). The fallback loads synchronously at
// construction (it's a local file) so the list is never empty; refresh()
// fetches the live half in the background (it's a blocking HTTP call) and
// merges it in -- live entries take priority over a fallback entry with
// the same reflector number, since the live one has a real, current host.
//
// Alongside them, the DPlus side: REF reflectors and DPlus repeater
// gateways (e.g. ZL2VH), from the list the DPlus authentication server
// sends when a callsign logs in (dplus::authenticate) -- so it can only be
// fetched once a callsign is set, and every DPlus connect refreshes it.

#include <QAbstractListModel>

#include <functional>
#include <vector>

#include "dplus_client.h"
#include "xlx_directory.h"

class ReflectorListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        HostRole = Qt::UserRole + 1,
        NameRole,
        IsDPlusRole, // bool: reached over DPlus rather than DExtra
    };

    explicit ReflectorListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // Kicks off an async refresh of the live reflector list. Safe to call
    // repeatedly; a refresh already in flight is left to finish and a new
    // one isn't started on top of it.
    void refresh();
    // The same for the DPlus list, logging in as callsign (see the top).
    void refreshDPlus(const QString &callsign);
    // A list fetched elsewhere -- by a DPlus connect, which logs in anyway.
    void setDPlusList(std::vector<dplus::GatewayEntry> list);

signals:
    void refreshFailed(const QString &error);

private:
    void applyLive(std::vector<xlx::ReflectorInfo> live);
    // Applies change (replacing one of the lists below) inside a model
    // reset and rebuilds m_rows. The change has to come after
    // beginResetModel(): m_rows points into those lists, and views (and
    // DStarTab's modelAboutToBeReset handler) read rows until then.
    void rebuildRows(const std::function<void()> &change = {});

    struct Row {
        const xlx::ReflectorInfo *info;
        bool dplus;
    };

    std::vector<xlx::ReflectorInfo> m_fallback;
    std::vector<xlx::ReflectorInfo> m_live;
    std::vector<xlx::ReflectorInfo> m_dplus;
    std::vector<Row> m_rows;
    bool m_refreshing = false;
    bool m_refreshingDPlus = false;
};
