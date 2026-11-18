#include "StreamMeshLink.h"

#include <QSerialPort>
#include <QTcpSocket>

namespace {
constexpr uchar kStart1 = 0x94, kStart2 = 0xC3;
constexpr int kMaxPacket = 512;
}

StreamMeshLink::StreamMeshLink(Kind kind, const QString &target, quint16 tcpPort, QObject *parent)
    : MeshLink(parent), m_kind(kind), m_target(target), m_port(tcpPort) {}

QString StreamMeshLink::describe() const {
    return m_kind == Serial ? tr("serial port %1").arg(m_target)
                            : tr("TCP %1:%2").arg(m_target).arg(m_port);
}

void StreamMeshLink::start() {
    if (m_kind == Serial) {
        auto *port = new QSerialPort(m_target, this);
        port->setBaudRate(115200);
        m_dev = port;
        connect(port, &QSerialPort::readyRead, this, &StreamMeshLink::onReadyRead);
        connect(port, &QSerialPort::errorOccurred, this, [this, port](QSerialPort::SerialPortError e) {
            if (e != QSerialPort::NoError && e != QSerialPort::TimeoutError) fail(port->errorString());
        });
        if (!port->open(QIODevice::ReadWrite)) {
            QMetaObject::invokeMethod(this, [this, port]() { fail(port->errorString()); }, Qt::QueuedConnection);
            return;
        }
        // wake the node's stream API: a run of START2 bytes makes it look for a frame
        port->write(QByteArray(32, char(kStart2)));
        QMetaObject::invokeMethod(this, [this]() { transportOpened(); }, Qt::QueuedConnection);
    } else {
        auto *sock = new QTcpSocket(this);
        m_dev = sock;
        connect(sock, &QTcpSocket::readyRead, this, &StreamMeshLink::onReadyRead);
        connect(sock, &QTcpSocket::connected, this, [this]() { transportOpened(); });
        connect(sock, &QTcpSocket::errorOccurred, this, [this, sock]() { fail(sock->errorString()); });
        connect(sock, &QTcpSocket::disconnected, this, [this]() { fail(tr("the node closed the connection")); });
        sock->connectToHost(m_target, m_port);
    }
}

void StreamMeshLink::writeToRadio(const QByteArray &toRadio) {
    if (!m_dev || !m_dev->isOpen()) return;
    QByteArray frame;
    frame.append(char(kStart1)).append(char(kStart2));
    frame.append(char((toRadio.size() >> 8) & 0xff)).append(char(toRadio.size() & 0xff));
    frame.append(toRadio);
    m_dev->write(frame);
}

void StreamMeshLink::onReadyRead() {
    m_buf.append(m_dev->readAll());
    for (;;) {
        // drop anything before a frame start (the node's debug text, line noise)
        int i = 0;
        while (i + 1 < m_buf.size() && !(uchar(m_buf[i]) == kStart1 && uchar(m_buf[i + 1]) == kStart2)) ++i;
        if (i) m_buf.remove(0, i);
        if (m_buf.size() < 4) return;
        int len = (uchar(m_buf[2]) << 8) | uchar(m_buf[3]);
        if (len > kMaxPacket) { m_buf.remove(0, 2); continue; } // not a real frame
        if (m_buf.size() < 4 + len) return;
        QByteArray msg = m_buf.mid(4, len);
        m_buf.remove(0, 4 + len);
        handleFromRadio(msg);
    }
}
