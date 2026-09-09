#pragma once

// Main window: incremental-search reflector picker, target module select,
// connect/disconnect toggle, and space-bar PTT. Owns the live session
// (ThumbDV + ALSA + DextraClient, and the capture/playback/network
// threads dextra_audio/dextra_client already define) for exactly one
// connection at a time -- the same shape as dextra_test's live mode, just
// driven by GUI events instead of argv and a blocking main().

#include <QMainWindow>

#include <atomic>
#include <memory>
#include <thread>

#include "dextra_audio.h"
#include "dextra_client.h"
#include "dvcontroller.h"
#include "settings.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSortFilterProxyModel;
class ReflectorListModel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    QString selectedHost() const;
    void onConnectClicked();
    void openSettings();

    void startConnect();
    void connectWorker(QString host, char targetModule, GuiSettings settings);
    void onConnectFinished(bool ok, QString error);

    void startDisconnect();
    void disconnectWorker();
    void onDisconnectFinished();

    // Blocking: stops the capture/playback/network threads and releases
    // the ThumbDV/ALSA/socket resources. Called from the worker thread on
    // a normal disconnect, and directly (accepting the brief block) from
    // closeEvent on app shutdown.
    void stopSessionBlocking();

    void setBusy(bool busy, const QString &status);
    void setConnected(bool connected, const QString &status);

    GuiSettings m_settings;

    QLineEdit *m_searchBox;
    QListView *m_reflectorList;
    ReflectorListModel *m_model;
    QSortFilterProxyModel *m_proxy;
    QComboBox *m_targetModule;
    QPushButton *m_connectButton;
    QPushButton *m_settingsButton;
    QLabel *m_statusLabel;

    // Live session state -- only meaningful while m_connected.
    std::unique_ptr<SerialDV::DVController> m_dv;
    dextra::AlsaPcm m_capture, m_playback;
    std::unique_ptr<dextra::DextraClient> m_client;
    dextra::PcmQueue m_rxQueue;
    std::thread m_captureThread, m_playbackThread, m_networkThread;
    std::thread m_worker; // the in-flight connect/disconnect sequence, if any
    std::atomic<bool> m_pttActive{false};
    bool m_connected = false;
    bool m_busy = false;
};
