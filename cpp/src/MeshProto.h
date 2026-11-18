#pragma once
// The few Meshtastic protobuf messages the gateway needs, encoded and decoded by hand
// (field numbers from meshtastic/protobufs mesh.proto). No protobuf library needed.
#include <QByteArray>
#include <QString>

namespace meshproto {

constexpr int kTextMessageApp = 1; // PortNum.TEXT_MESSAGE_APP

QByteArray toRadioWantConfig(quint32 configId);
QByteArray toRadioHeartbeat();
QByteArray toRadioText(quint32 to, quint32 packetId, const QByteArray &utf8, int hopLimit,
                       bool wantAck, quint32 channel = 0);

struct FromRadio {
    enum Kind { Other, Packet, MyInfo, NodeInfo, ConfigComplete, Rebooted };
    Kind kind = Other;
    quint32 myNodeNum = 0;   // MyInfo
    quint32 configId = 0;    // ConfigComplete
    // Packet
    quint32 from = 0, to = 0, packetId = 0, channel = 0;
    bool decoded = false;    // false: encrypted, not for us to read
    int portnum = -1;
    QByteArray payload;
    // NodeInfo
    quint32 nodeNum = 0;
    QString userId, longName, shortName;
};

bool parseFromRadio(const QByteArray &bytes, FromRadio &out);

inline QString nodeId(quint32 num) { return QStringLiteral("!%1").arg(num, 8, 16, QLatin1Char('0')); }
// "!0b7bbe91" -> number; returns false if it isn't a node ID
bool parseNodeId(const QString &id, quint32 &num);

} // namespace meshproto
