#pragma once

// YSF (System Fusion) tab: a searchable YSF reflector picker, connect/
// disconnect, PTT, the last stream's callsigns and a Last Heard list --
// the D-Star tab's shape, for YsfClient. One session at a time: ThumbDV
// (at DMR's rate, which is YSF DN mode's), ALSA streams, and the capture,
// playback and network threads, all stopped by ysf::g_running.

#include <atomic>
#include <memory>
#include <thread>

#include <QJsonObject>

#include "alsa_audio.h"
#include "dvcontroller.h"
#include "protocoltab.h"
#include "settings.h"
#include "vocoder_pipeline.h"
#include "ysf_client.h"

class AudioLevelsPanel;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTableWidget;
class YsfReflectorModel;

class YsfTab : public ProtocolTab {
    Q_OBJECT
public:
    explicit YsfTab(const GuiSettings &settings, QWidget *parent = nullptr);

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
    void setAutoLevel(bool on) override;

private:
    // The reflector picked from the list, or typed in full (a name or its
    // number); -1 if none.
    int selectedRow() const;
    void updateConnectButtonEnabled();

    void startConnect();
    void connectWorker(QString name, QString host, int port, GuiSettings settings);
    void onConnectFinished(bool ok, QString error, QString name);
    void onStreamStarted(ysf::StreamInfo info);
    void startDisconnect();
    void onDisconnectFinished();

    void setBusy(bool busy, const QString &status);
    void setConnected(bool connected, const QString &status);
    void updateVocoderStatus();

    // The DG-ID last used on each reflector (ysf_dgids.json), put back in
    // the DG-ID box when that reflector is picked: on reflectors with rooms
    // (JAPANLINK) connecting returns you to your last room and keying up
    // moves you to the box's, so the box has to follow the reflector or the
    // first over lands in the wrong room.
    void rememberedDgIdFor(const QString &reflector);

    // Favourites: reflector names, in ysf_favourites.json.
    bool isFavourite(const QString &name) const;
    void addFavouriteItem(const QString &name);
    void selectFavourite(const QString &name);
    void loadFavourites();
    void saveFavourites() const;
    void updateAddFavouriteEnabled();

    GuiSettings m_settings;

    QComboBox *m_reflectorCombo;
    YsfReflectorModel *m_model;
    QString m_pickBeforeReset; // kept across a list refresh, see the constructor
    QComboBox *m_dgIdBox = nullptr;
    QJsonObject m_dgIds;       // reflector name -> DG-ID
    QString m_dgIdReflector;   // the reflector the box's value belongs to
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_pttButton;
    QPushButton *m_addFavouriteButton = nullptr;
    QListWidget *m_favouritesList = nullptr;
    QLabel *m_statusLabel;
    QLabel *m_sourceLabel;
    QLabel *m_destinationLabel;
    QLabel *m_gatewayLabel;
    QLabel *m_modeLabel;
    QLabel *m_dgIdLabel;
    QTableWidget *m_lastHeardTable;
    AudioLevelsPanel *m_audioLevels;

    QString m_connectedName;

    // Live session state -- only meaningful while m_connected.
    std::unique_ptr<SerialDV::DVController> m_dv;
    std::unique_ptr<audio::VocoderPipeline> m_vocoder; // after m_dv: destroyed first
    audio::AlsaPcm m_capture, m_playback;
    std::unique_ptr<ysf::YsfClient> m_client;
    audio::PcmQueue m_rxQueue;
    uint8_t m_silence[ysf::AMBE_FRAME_SIZE] = {}; // an AMBE frame of silence, from the ThumbDV
    std::thread m_captureThread, m_playbackThread, m_networkThread;
    std::thread m_worker;
    std::atomic<bool> m_pttActive{false};
    bool m_connected = false;
    bool m_busy = false;
    bool m_lostConnection = false;
    QString m_connectedStatus;
    bool m_vocoderDown = false;
};
