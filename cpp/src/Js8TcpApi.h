#pragma once
// The TCP API of JS8Call or Subspace (port 2442): one JSON message per line. Replies carry
// the request's params._ID; everything else is an event.
#include "Js8Api.h"

#include <QHash>
#include <QTcpSocket>
#include <QTimer>

class Js8TcpApi : public Js8Api {
    Q_OBJECT
  public:
    Js8TcpApi(const QString &host, quint16 port, QObject *parent = nullptr);
    void start() override;
    QString describe() const override;
    bool isConnected() const override { return m_sock.state() == QAbstractSocket::ConnectedState; }
    bool isSubspace() const override { return m_subspace; }
    void request(const QString &type, const QString &value, const QVariantMap &params, Callback cb,
                 bool expectReply = true) override;

  private:
    struct Pending {
        Callback cb;
        bool expectReply;
        QString type;
        QTimer *timer;
    };
    void onLine(const QByteArray &line);
    void finish(qint64 id, const QJsonObject &reply, const QString &error);

    QString m_host;
    quint16 m_port;
    QTcpSocket m_sock;
    QByteArray m_buf;
    QHash<qint64, Pending> m_pending;
    qint64 m_nextId;
    bool m_subspace = false;
    QTimer m_retry;
};
