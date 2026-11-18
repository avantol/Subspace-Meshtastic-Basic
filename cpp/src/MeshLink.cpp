#include "MeshLink.h"
#include "MeshProto.h"

#include <QRandomGenerator>

namespace {
constexpr int kHeartbeatMs = 300 * 1000;   // as the Python library: keeps serial/TCP links alive
constexpr int kHandshakeMs = 90 * 1000;    // a large node database takes a while over BLE
}

MeshLink::MeshLink(QObject *parent) : QObject(parent) {
    m_heartbeat.setInterval(kHeartbeatMs);
    connect(&m_heartbeat, &QTimer::timeout, this, [this]() { writeToRadio(meshproto::toRadioHeartbeat()); });
    m_handshakeTimeout.setSingleShot(true);
    m_handshakeTimeout.setInterval(kHandshakeMs);
    connect(&m_handshakeTimeout, &QTimer::timeout, this,
            [this]() { fail(tr("the node didn't finish sending its configuration")); });
}

QString MeshLink::nodeName(quint32 num) const {
    QString id = meshproto::nodeId(num);
    QString name = m_names.value(num);
    return name.isEmpty() ? id : QStringLiteral("%1 (%2)").arg(id, name);
}

void MeshLink::transportOpened() {
    m_configId = QRandomGenerator::global()->bounded(1u, 0x7fffffffu);
    writeToRadio(meshproto::toRadioWantConfig(m_configId));
    m_handshakeTimeout.start();
}

void MeshLink::handleFromRadio(const QByteArray &bytes) {
    meshproto::FromRadio f;
    if (!meshproto::parseFromRadio(bytes, f)) return; // a damaged message; the next one will do
    switch (f.kind) {
    case meshproto::FromRadio::MyInfo:
        m_myNum = f.myNodeNum;
        break;
    case meshproto::FromRadio::NodeInfo:
        if (!f.shortName.isEmpty()) m_names.insert(f.nodeNum, f.shortName);
        break;
    case meshproto::FromRadio::ConfigComplete:
        if (f.configId == m_configId && !m_ready && m_myNum) {
            m_ready = true;
            m_handshakeTimeout.stop();
            m_heartbeat.start();
            emit ready(m_myNum);
        }
        break;
    case meshproto::FromRadio::Rebooted:
        fail(tr("the node rebooted"));
        break;
    case meshproto::FromRadio::Packet:
        if (m_ready && f.decoded && f.portnum == meshproto::kTextMessageApp)
            emit textReceived(f.from, f.to, QString::fromUtf8(f.payload));
        break;
    default:
        break;
    }
}

bool MeshLink::sendText(quint32 to, const QString &text, int hopLimit, bool wantAck) {
    if (!m_ready) return false;
    quint32 id = QRandomGenerator::global()->bounded(1u, 0xffffffffu);
    writeToRadio(meshproto::toRadioText(to, id, text.toUtf8(), hopLimit, wantAck));
    return true;
}

void MeshLink::fail(const QString &reason) {
    if (m_failed) return;
    m_failed = true;
    m_ready = false;
    m_heartbeat.stop();
    m_handshakeTimeout.stop();
    emit lost(reason);
}
