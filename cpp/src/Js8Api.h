#pragma once
// The JS8 JSON API of a Subspace Edition station (SubspaceRemoteApi: its phone-remote web
// server, over WiFi). Tests substitute a fake.
// Messages look like {"type":"RIG.GET_FREQ","value":"","params":{...}}.
#include <QJsonObject>
#include <QObject>
#include <QVariantMap>
#include <functional>

class Js8Api : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const QJsonObject &reply, const QString &error)>;
    using QObject::QObject;

    virtual void start() = 0;
    virtual QString describe() const = 0;
    virtual bool isConnected() const = 0;

    // Send one message. expectReply: wait for the reply and pass it to cb (error if none
    // comes). Otherwise cb gets an empty reply once sent, or an error if the station
    // refused it.
    virtual void request(const QString &type, const QString &value, const QVariantMap &params,
                         Callback cb, bool expectReply = true) = 0;

  signals:
    void event(const QJsonObject &message); // pushed by the station (RX.DIRECTED, TX.COMPLETE, ...)
    void connectedChanged(bool connected);
    void logMessage(const QString &text);
};
