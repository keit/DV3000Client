#pragma once

// Main window: hosts one tab per protocol (D-Star, DMR -- see DStarTab and
// DmrTab) in a QTabWidget, plus the menu bar (Settings, shared across both
// tabs since they read the same GuiSettings; Quit; log file location) and
// the space-bar PTT shortcut, routed to whichever tab is currently active
// via the ProtocolTab interface both implement.

#include <QMainWindow>

#include "settings.h"

class QAction;
class QTabWidget;
class DStarTab;
class DmrTab;
class ProtocolTab;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void openSettings();
    void updateSettingsActionEnabled();
    ProtocolTab *currentProtocolTab() const;

    GuiSettings m_settings;

    QTabWidget *m_tabs;
    DStarTab *m_dstarTab;
    DmrTab *m_dmrTab;
    QAction *m_settingsAction;
};
