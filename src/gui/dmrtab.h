#pragma once

// DMR tab: connect to a Homebrew/MMDVM master (BrandMeister or otherwise)
// using the DMR ID/password/server/color code from Settings, pick a
// talkgroup per-transmission (DMR has no per-connection "reflector" the
// way D-Star does -- see the project's protocol-roadmap notes on why
// RPTO/static TG assignment was skipped in favor of this), and PTT/last-
// heard controls mirroring DStarTab's shape. Owns its own live session
// (ThumbDV + ALSA + DmrClient, and dmr_audio's capture/playback threads)
// independently of DStarTab's -- the two are never expected to be
// connected at once (one physical ThumbDV), but nothing here assumes
// that beyond both trying to open the same device path.

#include <atomic>
#include <memory>
#include <thread>

#include "dextra_audio.h" // AlsaPcm, PcmQueue (protocol-agnostic, see dmr_audio.h)
#include "dmr_audio.h"
#include "dmr_client.h"
#include "dvcontroller.h"
#include "protocoltab.h"
#include "settings.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class TalkgroupListModel;

class DmrTab : public ProtocolTab {
    Q_OBJECT
public:
    explicit DmrTab(const GuiSettings &settings, QWidget *parent = nullptr);

    void applySettings(const GuiSettings &settings) { m_settings = settings; }

    QPushButton *pttButton() const override { return m_pttButton; }
    QPushButton *connectButton() const override { return m_connectButton; }
    QWidget *spaceExemptFocusWidget() const override;
    bool isActiveOrBusy() const override { return m_connected || m_busy; }
    void joinWorker() override {
        if (m_worker.joinable()) m_worker.join();
    }
    void stopSessionBlocking() override;

private:
    void onConnectClicked();

    void startConnect();
    void connectWorker(GuiSettings settings);
    void onConnectFinished(bool ok, QString error);

    void startDisconnect();
    void disconnectWorker();
    void onDisconnectFinished();

    void onHeaderReceived(uint32_t srcId, uint32_t dstId);
    void addLastHeardEntry(uint32_t srcId, uint32_t dstId);

    void setBusy(bool busy, const QString &status);
    void setConnected(bool connected, const QString &status);
    void updatePttButtonEnabled();

    // Extracts the leading run of digits from the talkgroup combo's
    // current text -- unlike DStarTab's reflector combo, any positive
    // integer is a legal talkgroup whether or not it's a row in the
    // directory, so this works equally for a picked row ("91 — World-
    // wide") and a number typed free-form that matches nothing listed.
    // Returns 0 if there's no leading digit yet (still mid-search).
    uint32_t currentTalkgroupId() const;

    GuiSettings m_settings;

    QComboBox *m_talkgroupCombo;
    TalkgroupListModel *m_talkgroupModel;
    QPushButton *m_connectButton;
    QPushButton *m_pttButton;
    QLabel *m_statusLabel;
    QTableWidget *m_lastHeardTable;

    // Live session state -- only meaningful while m_connected.
    std::unique_ptr<SerialDV::DVController> m_dv;
    dextra::AlsaPcm m_capture, m_playback;
    std::unique_ptr<dmr::DmrClient> m_client;
    dextra::PcmQueue m_rxQueue;
    std::thread m_captureThread, m_playbackThread, m_networkThread;
    std::thread m_worker; // the in-flight connect/disconnect sequence, if any
    std::atomic<bool> m_pttActive{false};
    // Read by the capture thread once per transmission (at the PTT-down
    // edge) via a lambda closing over this -- kept as an atomic, updated
    // from currentTalkgroupId() on every GUI-thread text change, rather
    // than calling currentTalkgroupId() (which touches the combo box)
    // directly from that thread, since QWidget isn't safe to touch off
    // the GUI thread.
    std::atomic<uint32_t> m_talkgroup{0};
    bool m_connected = false;
    bool m_busy = false;
};
