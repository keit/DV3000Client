#include <QApplication>

#include "mainwindow.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("DV3000Client");

    MainWindow window;
    window.show();

    return app.exec();
}
