#include "Settings.h"
#include "MeshProto.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

namespace {
// First run on a PC that ran the Python gateway: take its pass code and group choice.
void importPythonSettings(Settings &s) {
    QString dir = QDir::homePath() + "/.config/js8mesh/";
    QFile pass(dir + "subspace-passcode");
    if (s.subspacePasscode.isEmpty() && pass.open(QIODevice::ReadOnly))
        s.subspacePasscode = QString::fromUtf8(pass.readAll()).trimmed();
    QFile groups(dir + "forward-groups.json");
    if (groups.open(QIODevice::ReadOnly)) {
        QByteArray raw = groups.readAll().trimmed();
        QJsonDocument d = QJsonDocument::fromJson(raw);
        if (raw == "null") {
            s.groupsMode = Settings::GroupsAll; // JSON null = all groups
        } else if (d.isArray()) {
            s.groups.clear();
            for (const auto &v : d.array()) s.groups << v.toString();
            s.groupsMode = s.groups.isEmpty() ? Settings::GroupsNone : Settings::GroupsList;
        }
    }
}
} // namespace

void Settings::load() {
    QSettings q;
    if (!q.contains("mesh/type")) importPythonSettings(*this);
    meshType = MeshType(q.value("mesh/type", int(meshType)).toInt());
    bleDevice = q.value("mesh/bleDevice", bleDevice).toString();
    bleAddress = q.value("mesh/bleAddress", bleAddress).toString();
    serialPort = q.value("mesh/serialPort", serialPort).toString();
    meshTcpHost = q.value("mesh/tcpHost", meshTcpHost).toString();
    meshTcpPort = q.value("mesh/tcpPort", meshTcpPort).toInt();
    allowedNodes = q.value("mesh/allowedNodes", allowedNodes).toStringList();
    hopLimit = q.value("mesh/hopLimit", hopLimit).toInt();

    subspaceUrl = q.value("js8/subspaceUrl", subspaceUrl).toString();
    subspacePasscode = q.value("js8/subspacePasscode", subspacePasscode).toString();
    reloginSeconds = q.value("js8/reloginSeconds", reloginSeconds).toInt();

    groupsMode = GroupsMode(q.value("forward/groupsMode", int(groupsMode)).toInt());
    groups = q.value("forward/groups", groups).toStringList();

    inboxCount = q.value("limits/inboxCount", inboxCount).toInt();
    meshChunk = q.value("limits/meshChunk", meshChunk).toInt();
    meshGapSeconds = q.value("limits/meshGapSeconds", meshGapSeconds).toDouble();
    dedupeSeconds = q.value("limits/dedupeSeconds", dedupeSeconds).toInt();
    heardSeconds = q.value("limits/heardSeconds", heardSeconds).toInt();
    heardCount = q.value("limits/heardCount", heardCount).toInt();
    txReportSeconds = q.value("limits/txReportSeconds", txReportSeconds).toInt();

    startOnLaunch = q.value("app/startOnLaunch", startOnLaunch).toBool();
    closeToTray = q.value("app/closeToTray", closeToTray).toBool();
}

void Settings::save() const {
    QSettings q;
    q.setValue("mesh/type", int(meshType));
    q.setValue("mesh/bleDevice", bleDevice);
    q.setValue("mesh/bleAddress", bleAddress);
    q.setValue("mesh/serialPort", serialPort);
    q.setValue("mesh/tcpHost", meshTcpHost);
    q.setValue("mesh/tcpPort", meshTcpPort);
    q.setValue("mesh/allowedNodes", allowedNodes);
    q.setValue("mesh/hopLimit", hopLimit);
    q.remove("js8/type"); // from versions that also spoke JS8Call's TCP API
    q.remove("js8/host");
    q.remove("js8/port");
    q.setValue("js8/subspaceUrl", subspaceUrl);
    q.setValue("js8/subspacePasscode", subspacePasscode);
    q.setValue("js8/reloginSeconds", reloginSeconds);
    q.setValue("forward/groupsMode", int(groupsMode));
    q.setValue("forward/groups", groups);
    q.setValue("limits/inboxCount", inboxCount);
    q.setValue("limits/meshChunk", meshChunk);
    q.setValue("limits/meshGapSeconds", meshGapSeconds);
    q.setValue("limits/dedupeSeconds", dedupeSeconds);
    q.setValue("limits/heardSeconds", heardSeconds);
    q.setValue("limits/heardCount", heardCount);
    q.setValue("limits/txReportSeconds", txReportSeconds);
    q.setValue("app/startOnLaunch", startOnLaunch);
    q.setValue("app/closeToTray", closeToTray);
}

QString Settings::validate() const {
    if (meshType == MeshBle && bleDevice.trimmed().isEmpty()) return QObject::tr("Enter the Meshtastic node's Bluetooth name.");
    if (meshType == MeshSerial && serialPort.trimmed().isEmpty()) return QObject::tr("Choose the Meshtastic node's serial port.");
    if (meshType == MeshTcp && meshTcpHost.trimmed().isEmpty()) return QObject::tr("Enter the Meshtastic node's network address.");
    if (allowedNodes.isEmpty()) return QObject::tr("Add at least one allowed node (your handheld's node ID).");
    for (const QString &n : allowedNodes) {
        quint32 num;
        if (!meshproto::parseNodeId(n, num)) return QObject::tr("'%1' isn't a node ID (like !8a31cb4a).").arg(n);
    }
    if (!subspaceUrl.startsWith("http://") && !subspaceUrl.startsWith("https://"))
        return QObject::tr("The Subspace address should look like http://192.168.1.50:2443");
    if (subspacePasscode.trimmed().isEmpty()) return QObject::tr("Enter the Subspace phone-remote pass code.");
    return {};
}

QString Settings::groupsText() const {
    if (groupsMode == GroupsAll) return QObject::tr("Forwarding all @groups (not @HB).");
    if (groupsMode == GroupsNone || groups.isEmpty()) return QObject::tr("Forwarding no @groups (only messages to you).");
    QStringList g = groups;
    g.sort();
    return QObject::tr("Forwarding @groups: %1").arg(g.join(' '));
}
