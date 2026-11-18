#pragma once
// Meshtastic over a byte stream: USB serial, or TCP to a WiFi node (port 4403).
// Each protobuf is framed as 0x94 0xC3 <length, 2 bytes big-endian> <bytes>.
#include "MeshLink.h"

class QIODevice;

class StreamMeshLink : public MeshLink {
    Q_OBJECT
  public:
    enum Kind { Serial, Tcp };
    StreamMeshLink(Kind kind, const QString &target, quint16 tcpPort, QObject *parent = nullptr);
    void start() override;
    QString describe() const override;

  protected:
    void writeToRadio(const QByteArray &toRadio) override;

  private:
    void onReadyRead();
    Kind m_kind;
    QString m_target;  // serial port name, or TCP host
    quint16 m_port;
    QIODevice *m_dev = nullptr;
    QByteArray m_buf;
};
