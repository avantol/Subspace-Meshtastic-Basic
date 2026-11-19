#include "SubspaceRemoteApi.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {
constexpr int kReplyTimeoutMs = 15000;   // the server itself answers API.NO_REPLY after 10 s
constexpr int kNoReplyWaitMs = 3000;     // commands with no reply: the server holds the request open
constexpr int kRetryMs = 10000;          // event stream reconnect delay
constexpr int kWatchdogMs = 60000;       // the server sends a keep-alive every 15 s

// A network error in words. Qt 5 reports a timeout as "Operation canceled".
QString netError(QNetworkReply *r, const QString &url) {
    switch (r->error()) {
    case QNetworkReply::OperationCanceledError:
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    case QNetworkReply::TimeoutError:
#endif
        return QObject::tr("no answer from %1 (wrong address, not on the same network, a firewall, "
                           "or the phone remote is off in Subspace)").arg(url);
    case QNetworkReply::ConnectionRefusedError:
        return QObject::tr("%1 refused the connection (is the phone remote on in Subspace?)").arg(url);
    case QNetworkReply::HostNotFoundError:
        return QObject::tr("address %1 not found").arg(url);
    default:
        return r->errorString();
    }
}
}

SubspaceRemoteApi::SubspaceRemoteApi(const QString &url, const QString &passcode, int reloginSeconds, QObject *parent)
    : Js8Api(parent), m_url(url.trimmed()), m_passcode(passcode.trimmed()), m_reloginSeconds(reloginSeconds) {
    while (m_url.endsWith('/')) m_url.chop(1);
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &SubspaceRemoteApi::openEvents);
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(kWatchdogMs);
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        emit logMessage(tr("Subspace: event stream silent; reconnecting"));
        if (m_events) m_events->abort();
    });
}

SubspaceRemoteApi::~SubspaceRemoteApi() {
    if (m_events) {
        m_events->disconnect(this);
        m_events->abort();
    }
}

QString SubspaceRemoteApi::describe() const { return tr("Subspace phone remote %1").arg(m_url); }

void SubspaceRemoteApi::start() { openEvents(); }

void SubspaceRemoteApi::setUp(bool up) {
    if (up == m_eventsUp) return;
    m_eventsUp = up;
    emit connectedChanged(isConnected());
}

void SubspaceRemoteApi::withSession(std::function<void(const QString &)> then) {
    if (!m_session.isEmpty()) {
        then(QString());
        return;
    }
    if (m_kickedAt.isValid()) {
        qint64 wait = m_reloginSeconds - m_kickedAt.secsTo(QDateTime::currentDateTimeUtc());
        if (wait > 0) {
            then(tr("another device has the Subspace remote; retaking it in %1 s").arg(wait));
            return;
        }
    }
    m_waiting.append(then);
    if (m_loggingIn) return;
    m_loggingIn = true;

    QNetworkRequest req(QUrl(m_url + "/login"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(kReplyTimeoutMs);
    QJsonObject body{{"passcode", m_passcode}};
    QNetworkReply *r = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_loggingIn = false;
        int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString error;
        if (status == 403) error = tr("wrong pass code");
        else if (status == 429) error = tr("login locked for 60 s after wrong pass codes");
        else if (r->error() != QNetworkReply::NoError) error = netError(r, m_url);
        else {
            QByteArray s = QJsonDocument::fromJson(r->readAll()).object().value("session").toString().toUtf8();
            if (s.isEmpty()) error = tr("unexpected login reply");
            else {
                m_session = s;
                m_kickedAt = QDateTime();
                emit logMessage(tr("Subspace: logged in at %1").arg(m_url));
            }
        }
        auto waiting = m_waiting;
        m_waiting.clear();
        for (auto &w : waiting) w(error);
    });
}

void SubspaceRemoteApi::lostSession(const QByteArray &token, bool kicked) {
    if (m_session.isEmpty() || m_session != token) return;
    m_session.clear();
    if (!kicked) {
        // e.g. Subspace was restarted: the old session is simply gone; log in again now
        emit logMessage(tr("Subspace: session expired; logging in again"));
        return;
    }
    m_kickedAt = QDateTime::currentDateTimeUtc();
    emit logMessage(tr("Subspace: session ended (another login, e.g. a phone?); retaking it in %1 s")
                        .arg(m_reloginSeconds));
    emit connectedChanged(false);
}

void SubspaceRemoteApi::request(const QString &type, const QString &value, const QVariantMap &params,
                                Callback cb, bool expectReply) {
    withSession([=](const QString &error) {
        if (!error.isEmpty()) {
            cb({}, error);
            return;
        }
        QByteArray token = m_session;
        QNetworkRequest req(QUrl(m_url + "/api"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("X-Session", token);
        req.setTransferTimeout(expectReply ? kReplyTimeoutMs : kNoReplyWaitMs);
        QJsonObject msg{{"type", type}, {"value", value}, {"params", QJsonObject::fromVariantMap(params)}};
        QNetworkReply *r = m_nam.post(req, QJsonDocument(msg).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [=]() {
            r->deleteLater();
            int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 401) {
                lostSession(token, true);
                cb({}, tr("logged out of the Subspace remote"));
                return;
            }
            if (r->error() == QNetworkReply::OperationCanceledError || r->error() == QNetworkReply::TimeoutError) {
                // transfer timeout: fine for a command with no reply
                if (expectReply) cb({}, tr("%1: no reply from the station").arg(type));
                else cb({}, QString());
                return;
            }
            if (r->error() != QNetworkReply::NoError) {
                cb({}, r->errorString());
                return;
            }
            QJsonObject reply = QJsonDocument::fromJson(r->readAll()).object();
            if (reply.value("type").toString() == "API.NO_REPLY") {
                if (expectReply) cb({}, tr("%1: no reply from the station").arg(type));
                else cb({}, QString());
                return;
            }
            QJsonObject p = reply.value("params").toObject();
            if (p.value("REFUSED").toBool()) {
                cb(reply, tr("the station refused %1: %2").arg(type, p.value("REASON").toString()));
                return;
            }
            cb(reply, QString());
        });
    });
}

void SubspaceRemoteApi::openEvents() {
    if (m_events) return;
    withSession([this](const QString &error) {
        if (!error.isEmpty()) {
            emit logMessage(tr("Subspace: %1").arg(error));
            m_retry.start();
            return;
        }
        QByteArray token = m_session;
        QUrl url(m_url + "/events");
        QUrlQuery q;
        q.addQueryItem("s", QString::fromUtf8(token));
        url.setQuery(q);
        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        m_sseBuf.clear();
        m_events = m_nam.get(req);
        m_watchdog.start();
        connect(m_events, &QNetworkReply::metaDataChanged, this, [this]() {
            if (m_events && m_events->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
                emit logMessage(tr("Subspace: event stream open"));
                setUp(true);
            }
        });
        connect(m_events, &QNetworkReply::readyRead, this, [this]() {
            m_watchdog.start();
            m_sseBuf.append(m_events->readAll());
            int nl;
            while ((nl = m_sseBuf.indexOf('\n')) >= 0) {
                QByteArray line = m_sseBuf.left(nl).trimmed();
                m_sseBuf.remove(0, nl + 1);
                if (line.startsWith("data: ")) {
                    QJsonObject ev = QJsonDocument::fromJson(line.mid(6)).object();
                    if (!ev.isEmpty()) emit event(ev);
                }
            }
        });
        connect(m_events, &QNetworkReply::finished, this, [this, token]() {
            QNetworkReply *r = m_events;
            m_events = nullptr;
            m_watchdog.stop();
            int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QNetworkReply::NetworkError err = r->error();
            QString errText = r->errorString();
            r->deleteLater();
            setUp(false);
            if (status == 200 && err == QNetworkReply::NoError) {
                // the server closed the stream: another login ended our session
                lostSession(token, true);
            } else if (status == 401) {
                lostSession(token, false);
            } else if (err != QNetworkReply::NoError && err != QNetworkReply::OperationCanceledError) {
                emit logMessage(tr("Subspace: event stream error: %1").arg(errText));
            }
            m_retry.start();
        });
    });
}
