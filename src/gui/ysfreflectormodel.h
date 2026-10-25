#pragma once

// The YSF tab's reflector list: the public YSF reflector directory
// (Pi-Star's YSF_Hosts.txt, see ysf_client.h), loaded from the disk cache
// at startup and refreshed in the background at most once a day.

#include <QAbstractListModel>

#include <vector>

#include "ysf_client.h"

class YsfReflectorModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        HostRole = Qt::UserRole + 1,
        PortRole,
        NameRole,
    };

    explicit YsfReflectorModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // Fetches the list if the cached copy is over a day old.
    void refresh();

signals:
    void refreshFailed(const QString &error);

private:
    void setList(std::vector<ysf::ReflectorEntry> list);

    std::vector<ysf::ReflectorEntry> m_rows;
    bool m_refreshing = false;
};
