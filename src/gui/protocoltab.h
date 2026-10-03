#pragma once

// Common interface both DStarTab and DmrTab implement, so MainWindow can
// host either one inside a QTabWidget without knowing which protocol it
// is: routing the space-bar PTT shortcut to whichever tab is currently
// visible, disabling the shared Settings dialog while either tab has a
// live session or an in-flight connect/disconnect, and stopping that
// session cleanly on window close.

#include <QMetaObject>
#include <QSplitter>
#include <QWidget>

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
};
