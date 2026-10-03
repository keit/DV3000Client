#include <cstdio>

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QLoggingCategory>

#include "filelogging.h"
#include "mainwindow.h"

int main(int argc, char **argv) {
    // Qt's Wayland plugin logs a debug line every time the input-method
    // (text-input) connection moves between surfaces -- i.e. whenever a
    // popup or dialog takes or returns focus -- which just floods the log.
    // A QT_LOGGING_RULES set in the environment still takes precedence.
    QLoggingCategory::setFilterRules("qt.qpa.wayland.textinput=false\n");

    QApplication app(argc, argv);
    QApplication::setApplicationName("DV3000Client");
    QApplication::setApplicationVersion(DV3K_VERSION);

    // --version also lets CI smoke-test a packaged binary headless
    // (QT_QPA_PLATFORM=offscreen) without bringing up the window.
    QCommandLineParser parser;
    parser.setApplicationDescription("D-Star and DMR client for the ThumbDV (AMBE-3000)");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    // Window/taskbar icon, in the sizes desktops pick from (resources/
    // icons, built in via resources/dv3kclient.qrc; generated from the
    // 1024px original there). The desktop file name ties the running
    // window to dv3kclient.desktop (resources/, installed by the packages
    // or scripts/install-desktop-entry.sh), which is where GNOME on
    // Wayland takes the dock/Alt-Tab icon from.
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QString(":/icons/dv3kclient-%1.png").arg(size), QSize(size, size));
    }
    QApplication::setWindowIcon(icon);
    QGuiApplication::setDesktopFileName("dv3kclient");

    // Only safe to call after setApplicationName() above -- it determines
    // the log path via the same QStandardPaths::AppConfigLocation call
    // GuiSettings::filePath() uses for settings.json, which depends on the
    // application name already being set to know which subfolder to use.
    QString logPath = startFileLogging();
    if (!logPath.isEmpty()) {
        std::fprintf(stderr, "dv3kclient: logging to %s\n", logPath.toUtf8().constData());
    }

    MainWindow window;
    window.show();

    return app.exec();
}
