#pragma once

// DMR tab: connect to a Homebrew/MMDVM master (BrandMeister or otherwise)
// using the DMR ID/password/server/color code from Settings, pick a
// talkgroup per-transmission (DMR has no per-connection "reflector" the
// way D-Star does -- see the project's protocol-roadmap notes on why
// RPTO/static TG assignment was skipped in favor of this), and PTT/last-
// heard controls mirroring DStarTab's shape. The talkgroup combo can also
// address a Private (unit-to-unit) call to another DMR ID instead of a
// Group call to a talkgroup -- some network features require it, e.g.
// BrandMeister's Parrot echo test (ID 9990) only responds to a genuine
// Private call. Owns its own live session
// (ThumbDV + ALSA + DmrClient, and dmr_audio's capture/playback threads)
// independently of DStarTab's -- the two are never expected to be
// connected at once (one physical ThumbDV), but nothing here assumes
// that beyond both trying to open the same device path.

#include <atomic>
#include <memory>
#include <thread>

#include <QHash>

#include "dextra_audio.h" // AlsaPcm, PcmQueue (protocol-agnostic, see dmr_audio.h)
#include "dmr_audio.h"
#include "dmr_client.h"
#include "dvcontroller.h"
#include "protocoltab.h"
#include "settings.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
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

    // "CALLSIGN (id)" for a heard srcId, or just "id" if it's not (yet) in
    // m_dmrIdDirectory -- there's no callsign anywhere in the Homebrew
    // wire protocol itself, see dmriddirectory.h.
    QString displayCallsign(uint32_t dmrId) const;

    // "id — Name" for a heard dstId, using the same directory the
    // talkgroup combo's own entries come from, or just "id" if it's not
    // listed there (private-call target IDs, or an unlisted talkgroup).
    QString displayTalkgroup(uint32_t dstId) const;

    void setBusy(bool busy, const QString &status);
    void setConnected(bool connected, const QString &status);
    void updatePttButtonEnabled();

    // Records the last-transmitted target (id 0 = none) and refreshes the
    // label (which shows 0 and 4000 as "None") and the Add-to-favourites
    // button (enabled for 4000 too, so it can be saved as a favourite).
    void setActiveSubscription(uint32_t id, bool privateCall);
    void updateAddFavouriteEnabled();
    bool isFavourite(uint32_t id, bool privateCall) const;
    void addFavouriteItem(uint32_t id, bool privateCall);
    void removeFavouriteItem(QListWidgetItem *item);
    void selectFavourite(uint32_t id, bool privateCall);
    void loadFavourites();
    void saveFavourites() const;

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
    // When checked, the talkgroup combo's value is sent as a DMR ID for a
    // Private (unit-to-unit) call instead of a Group call to a talkgroup --
    // some network features (e.g. BrandMeister's Parrot echo test, ID
    // 9990) only respond to a genuine Private call.
    QCheckBox *m_privateCallCheck;
    QPushButton *m_connectButton;
    QPushButton *m_pttButton;
    QLabel *m_statusLabel;
    // BrandMeister has no persistent "connected to TG X" state -- the
    // login is to the master, not a talkgroup, and (per the project's
    // protocol-roadmap notes on skipping RPTO) it only relays a
    // talkgroup's traffic to you after you've transmitted to it, the same
    // "last used TG" convention real hotspots use. So your last-
    // transmitted TG is effectively your current one for RX too; this
    // label shows it, since it's otherwise invisible once PTT is released
    // and the combo box may have since been edited to something else.
    QLabel *m_activeTalkgroupLabel;
    QTableWidget *m_lastHeardTable;
    // The target shown in the "Current subscription" label (GUI thread
    // only): what the last transmission went to. 0 = none.
    uint32_t m_activeId = 0;
    bool m_activePrivate = false;
    QPushButton *m_addFavouriteButton;
    QListWidget *m_favouritesList;
    // Fetched once in the background at construction (see
    // dmriddirectory.h) -- read-only after that fetch completes, so safe
    // to read directly from the GUI thread without locking.
    QHash<uint32_t, QString> m_dmrIdDirectory;

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
    std::atomic<bool> m_privateCall{false};
    bool m_connected = false;
    bool m_busy = false;
};
