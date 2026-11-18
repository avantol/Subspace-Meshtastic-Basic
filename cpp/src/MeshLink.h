#pragma once
// A connection to a Meshtastic node: Bluetooth LE, USB serial or TCP. The subclasses move
// ToRadio/FromRadio protobufs; this class does the protocol (config handshake, heartbeat,
// text messages).
#include <QHash>
#include <QObject>
#include <QTimer>

class MeshLink : public QObject {
    Q_OBJECT
  public:
    explicit MeshLink(QObject *parent = nullptr);
    virtual void start() = 0;      // connect; emits ready() or lost()
    virtual QString describe() const = 0;
    bool isReady() const { return m_ready; }
    quint32 myNodeNum() const { return m_myNum; }
    QString nodeName(quint32 num) const;
    bool sendText(quint32 to, const QString &text, int hopLimit, bool wantAck = true);

  signals:
    void logMessage(const QString &text);
    void ready(quint32 myNodeNum);
    void lost(const QString &reason); // emitted once; the link is finished after it
    void textReceived(quint32 from, quint32 to, const QString &text);

  protected:
    virtual void writeToRadio(const QByteArray &toRadio) = 0;
    void transportOpened();                        // subclass: link is up, start the handshake
    void handleFromRadio(const QByteArray &bytes); // subclass: one FromRadio message arrived
    void fail(const QString &reason);              // subclass: link is down
    bool m_failed = false;

  private:
    bool m_ready = false;
    quint32 m_myNum = 0;
    quint32 m_configId = 0;
    QHash<quint32, QString> m_names;
    QTimer m_heartbeat, m_handshakeTimeout;
};
