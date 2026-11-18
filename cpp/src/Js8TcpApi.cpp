#include "Js8TcpApi.h"

#include <QDateTime>
#include <QJsonDocument>

namespace {
constexpr int kReplyTimeoutMs = 10000;
constexpr int kNoReplyWaitMs = 1500; // a refusal comes back at once; silence means accepted
constexpr int kRetryMs = 10000;
}

Js8TcpApi::Js8TcpApi(const QString &host, quint16 port, QObject *parent)
    : Js8Api(parent), m_host(host.trimmed()), m_port(port), m_nextId(QDateTime::currentMSecsSinceEpoch()) {
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &Js8TcpApi::start);
    connect(&m_sock, &QTcpSocket::readyRead, this, [this]() {
        m_buf.append(m_sock.readAll());
        int nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            QByteArray line = m_buf.left(nl).trimmed();
            m_buf.remove(0, nl + 1);
            if (!line.isEmpty()) onLine(line);
        }
    });
    connect(&m_sock, &QTcpSocket::connected, this, [this]() {
        emit logMessage(tr("JS8 API: connected to %1:%2").arg(m_host).arg(m_port));
        // Subspace answers MODE.GET_SUBMODE_NAME; stock JS8Call ignores it
        request("MODE.GET_SUBMODE_NAME", QString(), {}, [this](const QJsonObject &, const QString &error) {
            m_subspace = error.isEmpty();
            emit logMessage(m_subspace ? tr("JS8 API: the station is Subspace Edition")
                                       : tr("JS8 API: the station is JS8Call (no Subspace extensions)"));
            emit connectedChanged(true);
        });
    });
    connect(&m_sock, &QTcpSocket::disconnected, this, [this]() {
        emit logMessage(tr("JS8 API: disconnected"));
        for (qint64 id : m_pending.keys()) finish(id, {}, tr("disconnected"));
        emit connectedChanged(false);
        m_retry.start();
    });
    connect(&m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_sock.state() != QAbstractSocket::ConnectedState) {
            emit logMessage(tr("JS8 API: %1").arg(m_sock.errorString()));
            m_retry.start();
        }
    });
}

QString Js8TcpApi::describe() const { return tr("JS8 API %1:%2").arg(m_host).arg(m_port); }

void Js8TcpApi::start() {
    if (m_sock.state() != QAbstractSocket::UnconnectedState) m_sock.abort();
    m_buf.clear();
    m_sock.connectToHost(m_host, m_port);
}

void Js8TcpApi::request(const QString &type, const QString &value, const QVariantMap &params, Callback cb,
                        bool expectReply) {
    if (m_sock.state() != QAbstractSocket::ConnectedState) {
        cb({}, tr("not connected to %1").arg(describe()));
        return;
    }
    qint64 id = ++m_nextId;
    QVariantMap p = params;
    p.insert("_ID", id);
    QJsonObject msg{{"type", type}, {"value", value}, {"params", QJsonObject::fromVariantMap(p)}};
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id]() {
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;
        if (it->expectReply) finish(id, {}, tr("%1: no reply from the station").arg(it->type));
        else finish(id, {}, QString());
    });
    m_pending.insert(id, {cb, expectReply, type, timer});
    timer->start(expectReply ? kReplyTimeoutMs : kNoReplyWaitMs);
    m_sock.write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
}

void Js8TcpApi::finish(qint64 id, const QJsonObject &reply, const QString &error) {
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    Pending p = it.value();
    m_pending.erase(it);
    p.timer->deleteLater();
    p.cb(reply, error);
}

void Js8TcpApi::onLine(const QByteArray &line) {
    QJsonObject msg = QJsonDocument::fromJson(line).object();
    if (msg.isEmpty()) return;
    QJsonObject p = msg.value("params").toObject();
    qint64 id = p.value("_ID").toVariant().toLongLong();
    if (id > 0 && m_pending.contains(id)) {
        if (p.value("REFUSED").toBool())
            finish(id, msg, tr("the station refused %1: %2").arg(msg.value("type").toString(), p.value("REASON").toString()));
        else
            finish(id, msg, QString());
        return;
    }
    emit event(msg);
}
