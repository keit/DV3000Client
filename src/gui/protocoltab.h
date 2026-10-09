#pragma once

// Common interface both DStarTab and DmrTab implement, so MainWindow can
// host either one inside a QTabWidget without knowing which protocol it
// is: routing the space-bar PTT shortcut to whichever tab is currently
// visible, disabling the shared Settings dialog while either tab has a
// live session or an in-flight connect/disconnect, and stopping that
// session cleanly on window close.

#include <algorithm>
#include <cstdio>

#include <QMetaObject>
#include <QSplitter>
#include <QWidget>

#include "dvcontroller.h"
#include "ftdi_latency.h"
#include "settings.h"

class QPushButton;

class ProtocolTab : public QWidget {
    Q_OBJECT
public:
    explicit ProtocolTab(QWidget *parent = nullptr) : QWidget(parent) {}

    // The PTT toggle button space bar should drive when this tab is the
    // active one and no text-entry/connect widget has focus.
    virtual QPushButton *pttButton() const = 0;

    // The Connect/Disconnect button -- space bar activates it normally
    // (rather than being swallowed for PTT) when it has focus.
    virtual QPushButton *connectButton() const = 0;

    // The widget where space needs to type a literal space rather than
    // toggle PTT (a search/entry field), or nullptr if this tab has none.
    virtual QWidget *spaceExemptFocusWidget() const = 0;

    // True while a session is connected or a connect/disconnect is in
    // flight -- MainWindow disables the shared Settings dialog while
    // either tab reports true, since both tabs read the same audio/
    // ThumbDV device settings.
    virtual bool isActiveOrBusy() const = 0;

    // Blocking: waits out this tab's in-flight connect/disconnect worker
    // thread, if any -- must be called (from the GUI thread) before
    // stopSessionBlocking() below, since that thread is what writes this
    // tab's session members (m_dv/m_client/m_capture/...); joining it
    // first is what makes touching those members afterwards safe. Must
    // NOT be called from within the worker thread itself (e.g. from
    // inside a disconnectWorker() running as that very thread) -- joining
    // your own thread is undefined behavior.
    virtual void joinWorker() = 0;

    // Blocking: stops this tab's live session (capture/playback/network
    // threads, ThumbDV/ALSA/socket) if one is running. Called from
    // MainWindow::closeEvent on app shutdown, after joinWorker() above.
    virtual void stopSessionBlocking() = 0;

    // Sets this tab's mic and speaker volume sliders (0..100, 50 = unity)
    // and applies them to its audio devices, without emitting
    // volumesChanged() -- MainWindow uses it to sync the two tabs and to
    // apply the saved values at startup.
    virtual void setVolumes(int mic, int speaker) = 0;

    // Width of the left pane (controls) in this tab's left/Last-Heard
    // splitter -- MainWindow carries it from one tab to the next on a tab
    // switch, so the dividing border sits in the same place on both. The
    // splitter itself clamps to each tab's minimum width, so a width one
    // tab can't fit is simply the nearest it can.
    int leftPaneWidth() const { return m_splitter ? m_splitter->sizes().value(0) : 0; }
    void setLeftPaneWidth(int width) {
        if (!m_splitter) return;
        int right = m_splitter->width() - m_splitter->handleWidth() - width;
        m_splitter->setSizes({width, right > 0 ? right : 0});
    }

protected:
    // Set by each tab's constructor to its left/Last-Heard splitter.
    QSplitter *m_splitter = nullptr;

    // Called with the level meters (every 50 ms) while connected: watches
    // how long the ThumbDV's round trips take (see DVController::
    // recentEncodeMicros()) and emits thumbdvSlowChanged() when they're
    // too slow for real-time audio -- each 20 ms frame waits for its
    // reply, so much over 20 ms and audio falls behind and breaks up.
    // Raised after 2 s averaging over 20 ms, cleared after 2 s under
    // 17 ms; stretches with no audio at all count towards neither, so it
    // doesn't flicker between overs. Measured: a healthy ThumbDV behind an
    // AMBEServer runs 15-18.5 ms (one plugged in locally ~10-12 ms), the
    // same one with the Pi's latency timer at 16 ms 32-35 ms. Also logs the first round trips of
    // each session, to show what's normal for a given setup.
    void checkThumbdvTiming(const SerialDV::DVController &dv, bool network) {
        unsigned int encode = dv.recentEncodeMicros();
        unsigned int decode = dv.recentDecodeMicros();
        if (encode && !m_loggedEncodeTiming) {
            std::fprintf(stderr, "thumbdv: encode round trip %.1f ms\n", encode / 1000.0);
            m_loggedEncodeTiming = true;
        }
        if (decode && !m_loggedDecodeTiming) {
            std::fprintf(stderr, "thumbdv: decode round trip %.1f ms\n", decode / 1000.0);
            m_loggedDecodeTiming = true;
        }
        if (!encode && !decode) return;

        bool slow = encode > kSlowMicros || decode > kSlowMicros;
        bool fast = encode < kFastMicros && decode < kFastMicros;
        int &polls = m_thumbdvSlow ? m_fastPolls : m_slowPolls;
        polls = (m_thumbdvSlow ? fast : slow) ? polls + 1 : 0;
        if (polls < kPollsToChange) return;

        polls = 0;
        m_thumbdvSlow = !m_thumbdvSlow;
        // Only the directions with audio going through: 0 means idle (e.g.
        // no encoding while just listening), not instant.
        char figures[64];
        if (encode && decode)
            std::snprintf(figures, sizeof(figures), "%.1f ms encode / %.1f ms decode", encode / 1000.0, decode / 1000.0);
        else
            std::snprintf(figures, sizeof(figures), "%.1f ms %s", (encode ? encode : decode) / 1000.0,
                          encode ? "encode" : "decode");
        if (m_thumbdvSlow)
            std::fprintf(stderr, "thumbdv: round trips averaging %s -- over the 20 ms per frame real-time audio "
                                 "allows, so audio will break up\n", figures);
        else
            std::fprintf(stderr, "thumbdv: round trips back to %s\n", figures);
        emit thumbdvSlowChanged(m_thumbdvSlow, (std::max(encode, decode) + 500) / 1000, network);
    }

    // On each new connection: start the counts and first-round-trip logs
    // afresh. A warning already shown stays until round trips are seen to
    // be fast again.
    void resetThumbdvTiming() {
        m_slowPolls = m_fastPolls = 0;
        m_loggedEncodeTiming = m_loggedDecodeTiming = false;
    }

    // Called from a tab's connect worker thread right after its
    // DVController opened the ThumbDV: lowers a local FTDI's latency timer
    // to 1 ms if it can (see ftdi_latency.h) and reports the outcome on
    // the GUI thread via thumbdvLatencyChecked(). An AMBEServer target
    // reports ok -- its latency timer is the AMBEServer host's business.
    void checkThumbdvLatency(const GuiSettings &settings) {
        ftdi::LatencyStatus status;
        if (!settings.thumbdvIsNetwork()) status = ftdi::ensureLowLatency(settings.thumbdvDevice.toStdString());
        QMetaObject::invokeMethod(
            this,
            [this, status] {
                emit thumbdvLatencyChecked(status.ok(), QString::fromStdString(status.ttyName), status.latencyMs);
            },
            Qt::QueuedConnection);
    }

signals:
    void stateChanged(); // isActiveOrBusy() may have changed
    // The user moved one of this tab's volume sliders.
    void volumesChanged(int mic, int speaker);
    // ...and it has settled -- time to save.
    void volumesCommitted();
    // Outcome of checkThumbdvLatency() for the ThumbDV just opened: ok is
    // false when it's a local FTDI device still above 1 ms (latencyMs).
    void thumbdvLatencyChecked(bool ok, QString ttyName, int latencyMs);
    // See checkThumbdvTiming(): slow is whether round trips are too slow
    // for real-time audio, roundTripMs the slower direction's average, and
    // network whether the ThumbDV is behind an AMBEServer.
    void thumbdvSlowChanged(bool slow, int roundTripMs, bool network);

private:
    static constexpr unsigned int kSlowMicros = 20000;
    static constexpr unsigned int kFastMicros = 17000;
    static constexpr int kPollsToChange = 40; // 2 s of 50 ms polls
    bool m_thumbdvSlow = false;
    int m_slowPolls = 0;
    int m_fastPolls = 0;
    bool m_loggedEncodeTiming = false;
    bool m_loggedDecodeTiming = false;
};
