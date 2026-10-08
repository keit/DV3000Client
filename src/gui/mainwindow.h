#pragma once

// Main window: hosts one tab per protocol (D-Star, DMR -- see DStarTab and
// DmrTab) in a QTabWidget, plus the menu bar (Settings, shared across both
// tabs since they read the same GuiSettings; Quit; log file location) and
// the space-bar PTT shortcut, routed to whichever tab is currently active
// via the ProtocolTab interface both implement.

#include <QMainWindow>

#include "settings.h"

class QAction;
class QLabel;
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
    void onThumbdvLatencyChecked(bool ok, const QString &ttyName, int latencyMs);
    void onThumbdvSlowChanged(bool slow, int roundTripMs, bool network);

    GuiSettings m_settings;

    QTabWidget *m_tabs;
    int m_currentTabIndex = 0; // the tab being left, as of the next currentChanged
    DStarTab *m_dstarTab;
    DmrTab *m_dmrTab;
    QAction *m_settingsAction;
    // Shown above the tabs while the ThumbDV's FTDI latency timer is still
    // above 1 ms after the app's own attempt to lower it -- i.e. exactly
    // when the Getting Started page's udev rule is needed.
    QLabel *m_latencyBanner;
    // Shown while the ThumbDV's round trips are too slow for real-time
    // audio (ProtocolTab::checkThumbdvTiming()) -- e.g. a remote ThumbDV
    // whose latency timer the app can't set itself.
    QLabel *m_slowBanner;
};
