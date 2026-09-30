#pragma once

// D-Star tab: searchable reflector picker (an editable combo box with a
// QCompleter, rather than a separate search box + list -- typing filters a
// popup of matches, picking one collapses it back down), target module
// select, connect/disconnect toggle, and a sending/receiving toggle driven
// by either the PTT button or the space bar (routed here by MainWindow's
// event filter, via the ProtocolTab interface). Owns the live session
// (ThumbDV + ALSA + DextraClient, and the capture/playback/network
// threads dextra_audio/dextra_client already define) for exactly one
// connection at a time -- the same shape as dextra_test's live mode, just
// driven by GUI events instead of argv and a blocking main().
//
// Extracted from what used to be all of MainWindow, once DMR support
// needed a second, structurally different tab (see DmrTab) alongside this
// one -- see ProtocolTab for the interface MainWindow now hosts both
// through.

#include <atomic>
#include <memory>
#include <thread>

#include "dextra_audio.h"
#include "dextra_client.h"
#include "dvcontroller.h"
#include "protocoltab.h"
#include "settings.h"

class AudioLevelsPanel;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTableWidget;
class ReflectorListModel;

class DStarTab : public ProtocolTab {
    Q_OBJECT
public:
    explicit DStarTab(const GuiSettings &settings, QWidget *parent = nullptr);

    void applySettings(const GuiSettings &settings) { m_settings = settings; }

    QPushButton *pttButton() const override { return m_pttButton; }
    QPushButton *connectButton() const override { return m_connectButton; }
    QWidget *spaceExemptFocusWidget() const override;
    bool isActiveOrBusy() const override { return m_connected || m_busy; }
    void joinWorker() override {
        if (m_worker.joinable()) m_worker.join();
    }
    void stopSessionBlocking() override;
    void setVolumes(int mic, int speaker) override;

private:
    bool hasValidReflectorSelection() const;
    QString selectedHost() const;
    QString selectedReflectorName() const;
    void onConnectClicked();

    void startConnect();
    void connectWorker(QString host, QString reflectorName, char targetModule, GuiSettings settings);
    void onConnectFinished(bool ok, QString error, QString reflectorName, char targetModule);
    void onHeaderReceived(dextra::DStarHeader header);
    void addLastHeardEntry(const dextra::DStarHeader &header);

    void startDisconnect();
    void disconnectWorker();
    void onDisconnectFinished();

    void setBusy(bool busy, const QString &status);
    void setConnected(bool connected, const QString &status);
    // Polled with the level meters: while connected, flags a ThumbDV that
    // has stopped answering (e.g. an AMBEServer that went deaf) on the
    // status line, and clears it once it answers again.
    void updateVocoderStatus();
    void updateConnectButtonEnabled();

    // Favourites are reflector+module pairs, e.g. "XLX307" module "B".
    // Adding takes whatever's currently selected (valid reflector and a
    // real module); picking one loads both back into the combos, same as
    // choosing them by hand. Persisted to dstar_favourites.json, so they
    // survive restarts; removed via right-click or the Delete key.
    void updateAddFavouriteEnabled();
    bool isFavourite(const QString &reflectorName, char module) const;
    void addFavouriteItem(const QString &reflectorName, char module);
    void removeFavouriteItem(QListWidgetItem *item);
    void selectFavourite(const QString &reflectorName, char module);
    void loadFavourites();
    void saveFavourites() const;

    GuiSettings m_settings;

    QComboBox *m_reflectorCombo;
    ReflectorListModel *m_model;
    QComboBox *m_targetModule;
    AudioLevelsPanel *m_audioLevels;
    QPushButton *m_addFavouriteButton = nullptr;
    QListWidget *m_favouritesList = nullptr;
    QPushButton *m_connectButton;
    QPushButton *m_pttButton;
    QLabel *m_statusLabel;
    QLabel *m_rpt1Label;
    QLabel *m_rpt2Label;
    QLabel *m_urCallLabel;
    QLabel *m_myCallLabel;
    QLabel *m_myCall2Label;
    QTableWidget *m_lastHeardTable;

    // Which reflector/module m_lastHeardTable's entries are attributed to
    // -- set once per successful connect, since a session is only ever
    // linked to one reflector+module at a time, so every station heard
    // during it was necessarily heard via that same one.
    QString m_connectedReflectorName;
    char m_connectedModule = 0;

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
    // The status text setConnected(true) showed, to restore once the
    // ThumbDV answers again, and whether it's currently flagged as down.
    QString m_connectedStatus;
    bool m_vocoderDown = false;
};
