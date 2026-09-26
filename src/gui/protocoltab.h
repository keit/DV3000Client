#pragma once

// Common interface both DStarTab and DmrTab implement, so MainWindow can
// host either one inside a QTabWidget without knowing which protocol it
// is: routing the space-bar PTT shortcut to whichever tab is currently
// visible, disabling the shared Settings dialog while either tab has a
// live session or an in-flight connect/disconnect, and stopping that
// session cleanly on window close.

#include <QWidget>

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

signals:
    void stateChanged(); // isActiveOrBusy() may have changed
    // The user moved one of this tab's volume sliders.
    void volumesChanged(int mic, int speaker);
    // ...and it has settled -- time to save.
    void volumesCommitted();
};
