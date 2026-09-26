#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include "dmrtab.h"
#include "dstartab.h"
#include <utility>

#include "filelogging.h"
#include "protocoltab.h"
#include "settingsdialog.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_settings = GuiSettings::load();

    setWindowTitle("DV3000 Client");

    // Brushed-metal-style background, independently chosen (not sampled
    // from any other app) -- scoped to the QMainWindow selector only, so
    // it paints just the window's own background and doesn't cascade into
    // child widgets' native styling (each tab's own status colors
    // included). The focus rules below are separate, explicit type
    // selectors (QPushButton:focus etc.), so they're similarly scoped --
    // only widgets of those exact types get the thicker border, nothing
    // else picks up stray styling from this. A plain native focus
    // rectangle is easy to miss at a glance (especially tabbing through
    // with the window not front-of-mind); a solid colored border is much
    // harder not to notice.
    setStyleSheet(
        "QMainWindow {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "    stop:0 #eef0f2, stop:0.5 #cdd0d3, stop:1 #aeb2b6);"
        "}"
        "QPushButton:focus, QComboBox:focus, QLineEdit:focus {"
        "  border: 3px solid #2195f3;"
        "}");

    menuBar()->setStyleSheet("QMenuBar { background-color: #e8e8e8; }");
    auto *fileMenu = menuBar()->addMenu("&File");
    m_settingsAction = fileMenu->addAction("&Settings...", this, &MainWindow::openSettings);
    m_settingsAction->setShortcut(QKeySequence::Preferences);
    fileMenu->addSeparator();
    fileMenu->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);

    auto *helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&Log File Location...", [this] {
        QMessageBox::information(this, "Log File Location", logFilePath());
    });

    m_dstarTab = new DStarTab(m_settings);
    m_dmrTab = new DmrTab(m_settings);
    connect(m_dstarTab, &ProtocolTab::stateChanged, this, &MainWindow::updateSettingsActionEnabled);
    connect(m_dmrTab, &ProtocolTab::stateChanged, this, &MainWindow::updateSettingsActionEnabled);

    // Volume sliders live on both tabs but are one setting: moving either
    // pair updates the other tab and the saved settings (written once the
    // slider settles, not on every step). Applied to the tabs up front so
    // the saved levels are in effect from the first connect.
    for (auto [tab, other] : {std::pair<ProtocolTab *, ProtocolTab *>{m_dstarTab, m_dmrTab}, {m_dmrTab, m_dstarTab}}) {
        connect(tab, &ProtocolTab::volumesChanged, this, [this, other](int mic, int speaker) {
            m_settings.micVolume = mic;
            m_settings.speakerVolume = speaker;
            other->setVolumes(mic, speaker);
        });
        connect(tab, &ProtocolTab::volumesCommitted, this, [this] { m_settings.save(); });
        tab->setVolumes(m_settings.micVolume, m_settings.speakerVolume);
    }

    // Remember which network the DMR tab was on, to start there next time.
    connect(m_dmrTab, &DmrTab::networkChanged, this, [this](const QString &network) {
        if (network.isEmpty()) return;
        m_settings.dmrNetwork = network;
        m_settings.save();
    });

    m_tabs = new QTabWidget;
    m_tabs->addTab(m_dstarTab, "D-Star");
    m_tabs->addTab(m_dmrTab, "DMR");

    // Keep the left/right border in the same place on both tabs: on a
    // switch, the tab being shown takes the width the one being left had
    // (which reflects wherever the user dragged it). Deferred a tick --
    // the page being revealed hasn't been laid out at its real size yet,
    // so its splitter can't take a width until it has.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        auto *from = qobject_cast<ProtocolTab *>(m_tabs->widget(m_currentTabIndex));
        auto *to = qobject_cast<ProtocolTab *>(m_tabs->widget(index));
        m_currentTabIndex = index;
        if (!from || !to || from == to) return;
        int width = from->leftPaneWidth();
        QTimer::singleShot(0, to, [to, width] { to->setLeftPaneWidth(width); });
    });

    setCentralWidget(m_tabs);
    resize(900, 480);

    qApp->installEventFilter(this);
    updateSettingsActionEnabled();
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
}

ProtocolTab *MainWindow::currentProtocolTab() const {
    return qobject_cast<ProtocolTab *>(m_tabs->currentWidget());
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // Space toggles PTT on whichever tab is currently active, except:
        // while actually typing in that tab's search/entry field, where it
        // needs to type a literal space; and while that tab's Connect/
        // Disconnect button has focus, where it should do what a focused
        // QPushButton normally does on Space -- activate it -- rather than
        // have that get swallowed for PTT before it ever gets there. Only
        // the press toggles -- the release is still swallowed (returning
        // true for both) so it can't leak through as e.g. activating
        // whatever widget happens to have focus.
        // Being an application-wide filter, this would otherwise also
        // intercept space bar inside any modal dialog on top of the main
        // window (Settings, a QMessageBox, ...) -- e.g. typing a space
        // into a Settings field would get swallowed as a PTT toggle for
        // whichever tab is still sitting behind it, instead of reaching
        // the field at all. Skip entirely whenever a modal is active;
        // this shortcut only makes sense when the main window itself
        // actually has the user's attention.
        ProtocolTab *tab = currentProtocolTab();
        if (tab && !QApplication::activeModalWidget() && ke->key() == Qt::Key_Space && !ke->isAutoRepeat() &&
            qApp->focusWidget() != tab->spaceExemptFocusWidget() && qApp->focusWidget() != tab->connectButton()) {
            if (event->type() == QEvent::KeyPress && tab->pttButton()->isEnabled()) tab->pttButton()->toggle();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // Wait out any in-flight connect/disconnect before touching either
    // tab's session members -- see ProtocolTab::joinWorker's comment.
    m_dstarTab->joinWorker();
    m_dmrTab->joinWorker();
    m_dstarTab->stopSessionBlocking();
    m_dmrTab->stopSessionBlocking();
    QMainWindow::closeEvent(event);
}

void MainWindow::openSettings() {
    SettingsDialog dlg(m_settings, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_settings = dlg.settings();
        m_settings.save();
        m_dstarTab->applySettings(m_settings);
        m_dmrTab->applySettings(m_settings);
    }
}

void MainWindow::updateSettingsActionEnabled() {
    m_settingsAction->setEnabled(!m_dstarTab->isActiveOrBusy() && !m_dmrTab->isActiveOrBusy());
}
