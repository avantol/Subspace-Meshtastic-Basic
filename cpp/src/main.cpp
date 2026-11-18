// JS8 Mesh Gateway: lets a Meshtastic handheld use a Subspace Edition JS8 station.
//   --start      start the gateway at once
//   --minimized  start hidden in the system tray
#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("js8mesh");
    QApplication::setApplicationName("js8mesh");
    QApplication::setApplicationDisplayName("JS8 Mesh Gateway");
    QApplication::setApplicationVersion(JS8MESH_VERSION);
    QApplication::setQuitOnLastWindowClosed(false); // the tray icon may keep it running

    // one copy at a time: two gateways would fight over the node and the station
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QLockFile lock(dir + "/js8mesh.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) {
        QMessageBox::information(nullptr, "JS8 Mesh Gateway",
                                 QObject::tr("JS8 Mesh Gateway is already running (look in the system tray)."));
        return 0;
    }

    QStringList args = app.arguments();
    MainWindow w;
    if (!args.contains("--minimized")) w.show();
    Settings s;
    s.load();
    if (args.contains("--start") || s.startOnLaunch) w.startGateway();
    return app.exec();
}
