#pragma once
// Everything the user can configure. Stored with QSettings (registry on Windows,
// ~/.config/js8mesh/js8mesh.conf on Linux).
#include <QString>
#include <QStringList>

struct Settings {
    // Meshtastic node
    enum MeshType { MeshBle, MeshSerial, MeshTcp };
    MeshType meshType = MeshBle;
    QString bleDevice;              // Bluetooth name or address, e.g. "Rpt1_be91"
    QString serialPort;             // e.g. "COM5" or "/dev/ttyACM0"
    QString meshTcpHost;            // a WiFi node's address
    int meshTcpPort = 4403;
    QStringList allowedNodes;       // node IDs like "!8a31cb4a"
    int hopLimit = 3;

    // Subspace station
    QString subspaceUrl = QStringLiteral("http://192.168.1.50:2443");
    QString subspacePasscode;
    int reloginSeconds = 120;       // after a phone takes the Subspace remote, wait before retaking it

    // forwarding of received JS8 messages
    enum GroupsMode { GroupsNone, GroupsAll, GroupsList };
    GroupsMode groupsMode = GroupsNone;
    QStringList groups;             // with GroupsList, e.g. "@MAGNET"

    // limits
    int inboxCount = 5;             // INBOX lists this many
    int meshChunk = 200;            // max characters per mesh message
    double meshGapSeconds = 3.0;    // between outgoing mesh messages (LoRa airtime)
    int dedupeSeconds = 600;        // drop a repeat of the same JS8 message within this time
    int heardSeconds = 3600;        // HEARD: stations heard within this time...
    int heardCount = 8;             // ...up to this many
    int txReportSeconds = 300;      // how long to wait for a transmission before reporting

    // app
    bool startOnLaunch = false;     // start the gateway when the app opens
    bool closeToTray = true;        // closing the window keeps the gateway running in the tray

    void load();
    void save() const;
    QString validate() const;       // empty if usable, else what to fix
    QString groupsText() const;     // "Forwarding @groups: ..." as GROUPS replies
};
