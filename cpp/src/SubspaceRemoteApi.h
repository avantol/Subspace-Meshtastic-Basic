#pragma once
// A Subspace Edition station's phone remote (File > Phone remote), port 2443 by default:
//   POST /login   {"passcode":"..."} -> {"ok":true,"session":"<token>"}
//   POST /api     one API message in, its reply out (X-Session header)
//   GET  /events  Server-Sent Events: every broadcast (?s=<token>)
// One session at a time: a new login (e.g. from a phone) ends ours. We then wait
// reloginSeconds before logging back in, which logs the other device out.
#include "Js8Api.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

class QNetworkReply;

class SubspaceRemoteApi : public Js8Api {
    Q_OBJECT
  public:
    SubspaceRemoteApi(const QString &url, const QString &passcode, int reloginSeconds, QObject *parent = nullptr);
    ~SubspaceRemoteApi() override;
    void start() override;
    QString describe() const override;
    bool isConnected() const override { return m_eventsUp && !m_session.isEmpty(); }
    void request(const QString &type, const QString &value, const QVariantMap &params, Callback cb,
                 bool expectReply = true) override;

  private:
    void withSession(std::function<void(const QString &error)> then);
    void lostSession(const QByteArray &token, bool kicked);
    void openEvents();
    void eventsEnded();
    void setUp(bool up);

    QNetworkAccessManager m_nam;
    QString m_url, m_passcode;
    int m_reloginSeconds;
    QByteArray m_session;
    QDateTime m_kickedAt;
    bool m_loggingIn = false;
    QList<std::function<void(const QString &)>> m_waiting; // callers waiting for a login
    QPointer<QNetworkReply> m_events;
    QByteArray m_sseBuf;
    bool m_eventsUp = false;
    QTimer m_retry, m_watchdog;
};
