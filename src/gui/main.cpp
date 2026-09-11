#include <cstdio>

#include <QApplication>

#include "filelogging.h"
#include "mainwindow.h"

int main(int argc, char **argv) {
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
