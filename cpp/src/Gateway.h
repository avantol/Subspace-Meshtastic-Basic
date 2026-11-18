#pragma once
// The gateway: commands from allowed Meshtastic nodes (DMs) drive the JS8 station, and
// JS8 messages for the user's callsign (and chosen @groups) are DMed back to them.
// Command list: COMMANDS.md.
#include "Settings.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <functional>

class Js8Api;
class MeshLink;
class QJsonObject;

class Gateway : public QObject {
    Q_OBJECT
  public:
    explicit Gateway(QObject *parent = nullptr);
    ~Gateway() override;

    void start(const Settings &settings);
    void stop();
    bool isRunning() const { return m_running; }
    const Settings &settings() const { return m_s; }

    // Handle one command from an allowed node. Public for tests.
    void handleCommand(const QString &sender, const QString &text);
    // Tests: use this API instead of connecting to a station.
    void setApiForTest(Js8Api *api, const Settings &settings);

  signals:
    void logMessage(const QString &line);
    void meshStatus(const QString &text, bool ok);
    void js8Status(const QString &text, bool ok);
    void settingsChanged(const Settings &settings); // e.g. GROUPS changed the forwarded groups
    void meshQueued(const QString &dest, const QString &text); // every outgoing DM (tests)

  private:
    struct PendingTx {
        QString sender, label, expect, status;
        QDateTime deadline;
    };
    using Reply = std::function<void(const QJsonObject &reply, const QString &error)>;

    void log(const QString &text);
    void wireApi();
    void startMesh();
    void onMeshLost(const QString &reason);
    void meshSend(const QString &dest, const QString &text);
    void pumpMesh();
    void onJs8Event(const QJsonObject &ev);
    void onDirected(const QJsonObject &ev);
    void refreshStation();

    // command helpers
    void reply(const QString &to, const QString &text) { meshSend(to, text); }
    void request(const QString &type, const QString &value, const QVariantMap &params, Reply cb,
                 bool expectReply = true);
    void sendDirected(const QString &sender, const QString &dest, const QString &body, const QString &label);
    void selectAndThen(const QString &sender, const QString &call, std::function<void()> then);
    void waitForSelection(const QString &want, int tries, std::function<void(bool ok, const QString &got)> done);
    void press(const QString &sender, const QString &action, const QString &expect, const QString &label);
    void withTxWarning(std::function<void(const QString &warning)> then);
    void trackTx(const QString &sender, const QString &label, const QString &expect);
    void checkTx();
    QString bare(const QString &text) const;
    void cmdStatus(const QString &sender);
    void cmdHeard(const QString &sender);
    void cmdInbox(const QString &sender);

    Settings m_s;
    bool m_running = false;
    Js8Api *m_api = nullptr;
    bool m_ownsApi = true;
    QPointer<MeshLink> m_mesh;
    QString m_bleAddress;           // learned on the first BLE connection (Linux: clear a stale link)
    QTimer m_meshRetry, m_meshGap, m_txTick, m_stationRefresh;
    QQueue<QPair<QString, QString>> m_meshOut;
    QString m_callsign, m_grid;
    QHash<QString, QDateTime> m_recent; // dedupe of forwarded JS8 messages
    QList<PendingTx> m_tx;
};
