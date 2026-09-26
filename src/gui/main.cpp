#include <cstdio>

#include <QApplication>
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

    // Only safe to call after setApplicationName() above -- it determines
    // the log path via the same QStandardPaths::AppConfigLocation call
    // GuiSettings::filePath() uses for settings.json, which depends on the
    // application name already being set to know which subfolder to use.
    QString logPath = startFileLogging();
    if (!logPath.isEmpty()) {
        std::fprintf(stderr, "dv3000client_gui: logging to %s\n", logPath.toUtf8().constData());
    }

    MainWindow window;
    window.show();

    return app.exec();
}
