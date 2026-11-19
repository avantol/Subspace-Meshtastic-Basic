#include "Gateway.h"

#include "BleMeshLink.h"
#include "MeshProto.h"
#include "StreamMeshLink.h"
#include "SubspaceRemoteApi.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <iterator>

namespace {
constexpr int kMeshRetryMs = 15000;
constexpr int kMeshQueueMax = 60;
constexpr int kOffsetMin = 300, kOffsetMax = 2700;

// Short forms: first two letters of each command. HE is HEARD; HELP is H or ?.
const QHash<QString, QString> kAliases = {
    {"SE", "SEND"}, {"IN", "INBOX"}, {"HE", "HEARD"}, {"ST", "STATUS"}, {"FR", "FREQ"},
    {"OF", "OFFSET"}, {"PI", "PING"}, {"GR", "GROUPS"}, {"CA", "CALL"}, {"RE", "REPLY"},
    {"SN", "SNR"}, {"AG", "AGAIN"}, {"H", "HELP"}, {"?", "HELP"}};

const QRegularExpression kCallRe(
    QStringLiteral("^(@[A-Z0-9]{1,8}|[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}(/[A-Z0-9]{1,4})?)$"));
const QRegularExpression kGroupRe(QStringLiteral("^@[A-Z0-9]{1,8}$"));

// A callsign or @group. A callsign needs a letter, so "73" is message text.
bool isCall(const QString &w) {
    if (!kCallRe.match(w).hasMatch()) return false;
    if (w.startsWith('@')) return true;
    return std::any_of(w.begin(), w.end(), [](QChar c) { return c.isLetter(); });
}

// The text after the first n words, spacing kept.
QString restAfter(const QString &text, int n) {
    static const QRegularExpression word(QStringLiteral("^\\s*\\S+"));
    QString s = text;
    for (int i = 0; i < n; ++i) {
        auto m = word.match(s);
        if (!m.hasMatch()) return {};
        s = s.mid(m.capturedEnd());
    }
    return s.trimmed();
}

struct Band { const char *name; qint64 lo, hi; };
const Band kBands[] = {
    {"2190m", 136000, 137000}, {"630m", 472000, 479000}, {"560m", 501000, 504000},
    {"160m", 1800000, 2000000}, {"80m", 3500000, 4000000}, {"60m", 5060000, 5450000},
    {"40m", 7000000, 7300000}, {"30m", 10000000, 10150000}, {"20m", 14000000, 14350000},
    {"17m", 18068000, 18168000}, {"15m", 21000000, 21450000}, {"12m", 24890000, 24990000},
    {"10m", 28000000, 29700000}, {"6m", 50000000, 54000000}, {"4m", 70000000, 71000000},
    {"2m", 144000000, 148000000}, {"1.25m", 222000000, 225000000}, {"70cm", 420000000, 450000000},
    {"33cm", 902000000, 928000000}, {"23cm", 1240000000, 1300000000}, {"13cm", 2300000000LL, 2450000000LL}};

QString bandOf(qint64 hz) {
    for (const Band &b : kBands)
        if (hz >= b.lo && hz <= b.hi) return QString::fromLatin1(b.name);
    return {};
}

QString ascii(const QString &s) {
    static const QRegularExpression nonAscii(QStringLiteral("[^\\x20-\\x7e]+"));
    QString t = s;
    return t.replace(nonAscii, " ").simplified();
}

QString ago(qint64 msUtc) {
    return QStringLiteral("%1m").arg((QDateTime::currentMSecsSinceEpoch() - msUtc) / 60000);
}
} // namespace

Gateway::Gateway(QObject *parent) : QObject(parent) {
    m_meshRetry.setSingleShot(true);
    m_meshRetry.setInterval(kMeshRetryMs);
    connect(&m_meshRetry, &QTimer::timeout, this, &Gateway::startMesh);
    m_meshGap.setSingleShot(true);
    connect(&m_meshGap, &QTimer::timeout, this, &Gateway::pumpMesh);
    m_txTick.setInterval(2000);
    connect(&m_txTick, &QTimer::timeout, this, &Gateway::checkTx);
    m_stationRefresh.setInterval(10 * 60 * 1000);
    connect(&m_stationRefresh, &QTimer::timeout, this, &Gateway::refreshStation);
}

Gateway::~Gateway() { stop(); }

void Gateway::log(const QString &text) {
    emit logMessage(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss ") + text);
}

// ---- lifecycle -----------------------------------------------------------------------
void Gateway::start(const Settings &settings) {
    stop();
    m_s = settings;
    m_running = true;
    m_api = new SubspaceRemoteApi(m_s.subspaceUrl, m_s.subspacePasscode, m_s.reloginSeconds, this);
    m_ownsApi = true;
    wireApi();
    log(tr("Connecting to %1...").arg(m_api->describe()));
    emit js8Status(tr("connecting to %1").arg(m_api->describe()), false);
    m_api->start();
    startMesh();
    m_txTick.start();
}

void Gateway::setApiForTest(Js8Api *api, const Settings &settings) {
    m_s = settings;
    m_api = api;
    m_ownsApi = false;
    m_running = true;
    wireApi();
}

void Gateway::stop() {
    m_running = false;
    m_meshRetry.stop();
    m_meshGap.stop();
    m_txTick.stop();
    m_stationRefresh.stop();
    m_meshOut.clear();
    m_tx.clear();
    if (m_mesh) {
        m_mesh->disconnect(this);
        delete m_mesh;
    }
    if (m_api) {
        m_api->disconnect(this);
        if (m_ownsApi) delete m_api;
        m_api = nullptr;
    }
    m_callsign.clear();
}

void Gateway::wireApi() {
    connect(m_api, &Js8Api::logMessage, this, &Gateway::log);
    connect(m_api, &Js8Api::event, this, &Gateway::onJs8Event);
    connect(m_api, &Js8Api::connectedChanged, this, [this](bool up) {
        if (up) refreshStation();
        else emit js8Status(tr("not connected (%1)").arg(m_api->describe()), false);
    });
}

void Gateway::refreshStation() {
    request("STATION.GET_CALLSIGN", {}, {}, [this](const QJsonObject &r, const QString &err) {
        if (!err.isEmpty()) {
            log(tr("Couldn't read the station callsign: %1").arg(err));
            return;
        }
        bool first = m_callsign.isEmpty();
        m_callsign = r.value("value").toString().trimmed().toUpper();
        if (first) log(tr("JS8 station connected as %1").arg(m_callsign));
        emit js8Status(tr("%1 as %2").arg(m_api->describe(), m_callsign), true);
        m_stationRefresh.start();
        request("STATION.GET_GRID", {}, {}, [this](const QJsonObject &g, const QString &e) {
            if (e.isEmpty()) m_grid = g.value("value").toString().trimmed().toUpper();
        });
    });
}

void Gateway::startMesh() {
    if (!m_running) return;
    MeshLink *link = nullptr;
    switch (m_s.meshType) {
    case Settings::MeshBle:
#ifdef Q_OS_LINUX
        // a link left over from a killed run stops the node advertising: drop it first
        if (!m_bleAddress.isEmpty())
            QProcess::execute("bluetoothctl", {"disconnect", m_bleAddress});
#endif
        link = new BleMeshLink(m_s.bleDevice, m_s.bleAddress, this);
        break;
    case Settings::MeshSerial:
        link = new StreamMeshLink(StreamMeshLink::Serial, m_s.serialPort, 0, this);
        break;
    case Settings::MeshTcp:
        link = new StreamMeshLink(StreamMeshLink::Tcp, m_s.meshTcpHost, quint16(m_s.meshTcpPort), this);
        break;
    }
    m_mesh = link;
    connect(link, &MeshLink::logMessage, this, &Gateway::log);
    connect(link, &MeshLink::ready, this, [this, link](quint32 num) {
        if (auto *ble = qobject_cast<BleMeshLink *>(link)) m_bleAddress = ble->address();
        log(tr("Meshtastic connected (%1, node %2). Allowed nodes: %3")
                .arg(link->describe(), meshproto::nodeId(num), m_s.allowedNodes.join(", ")));
        emit meshStatus(tr("%1, node %2").arg(link->describe(), meshproto::nodeId(num)), true);
        pumpMesh();
    });
    connect(link, &MeshLink::lost, this, &Gateway::onMeshLost);
    connect(link, &MeshLink::textReceived, this, [this, link](quint32 from, quint32 to, const QString &text) {
        QString sender = meshproto::nodeId(from);
        if (to != link->myNodeNum()) { // channel traffic, not a DM to the gateway
            if (m_s.allowedNodes.contains(sender, Qt::CaseInsensitive))
                log(tr("MESH channel text from %1 ignored (commands must be a DM to the gateway's node): '%2'")
                        .arg(link->nodeName(from), text));
            return;
        }
        log(tr("MESH RX <- %1 '%2'").arg(link->nodeName(from), text));
        if (!m_s.allowedNodes.contains(sender, Qt::CaseInsensitive)) {
            log(tr("  ignored: sender not in the allowed nodes"));
            return;
        }
        handleCommand(sender, text.trimmed());
    });
    log(tr("Connecting to the Meshtastic node (%1)...").arg(link->describe()));
    emit meshStatus(tr("connecting (%1)").arg(link->describe()), false);
    link->start();
}

void Gateway::onMeshLost(const QString &reason) {
    log(tr("Meshtastic link down: %1. Retrying in %2 s.").arg(reason).arg(kMeshRetryMs / 1000));
    emit meshStatus(tr("down: %1").arg(reason), false);
    if (m_mesh) {
        m_mesh->disconnect(this);
        m_mesh->deleteLater();
        m_mesh = nullptr;
    }
    if (m_running) m_meshRetry.start();
}

// ---- mesh output -----------------------------------------------------------------------
void Gateway::meshSend(const QString &dest, const QString &text) {
    QString t = text.trimmed();
    int chunk = std::max(20, m_s.meshChunk);
    QStringList pieces;
    while (t.size() > chunk) {
        // break at the last space that fits, leaving room for the "(n/m) " prefix
        int limit = chunk - 8;
        int cut = t.lastIndexOf(' ', limit);
        if (cut < limit / 2) cut = limit; // one very long word: cut it
        pieces << t.left(cut).trimmed();
        t = t.mid(cut).trimmed();
    }
    if (!t.isEmpty() || pieces.isEmpty()) pieces << t;
    for (int n = 0; n < pieces.size(); ++n) {
        QString piece = pieces.size() > 1 ? QStringLiteral("(%1/%2) %3").arg(n + 1).arg(pieces.size()).arg(pieces[n])
                                          : pieces[n];
        if (m_meshOut.size() >= kMeshQueueMax) {
            log(tr("MESH queue full; dropped: '%1'").arg(piece));
            continue;
        }
        m_meshOut.enqueue({dest, piece});
        emit meshQueued(dest, piece);
    }
    if (!m_meshGap.isActive()) pumpMesh();
}

void Gateway::pumpMesh() {
    if (m_meshOut.isEmpty() || m_meshGap.isActive() || !m_mesh || !m_mesh->isReady()) return;
    auto [dest, text] = m_meshOut.dequeue();
    quint32 num = 0;
    meshproto::parseNodeId(dest, num);
    if (m_mesh->sendText(num, text, m_s.hopLimit))
        log(tr("MESH TX -> %1 '%2'").arg(dest, text));
    m_meshGap.start(int(m_s.meshGapSeconds * 1000));
}

// ---- JS8 side --------------------------------------------------------------------------
void Gateway::request(const QString &type, const QString &value, const QVariantMap &params, Reply cb,
                      bool expectReply) {
    if (!m_api) {
        cb({}, tr("not connected to a JS8 station"));
        return;
    }
    m_api->request(type, value, params, cb, expectReply);
}

void Gateway::onJs8Event(const QJsonObject &ev) {
    QString type = ev.value("type").toString();
    QJsonObject p = ev.value("params").toObject();
    if (type == "RX.DIRECTED") {
        onDirected(ev);
    } else if (type == "TX.FRAME") {
        for (auto &t : m_tx) {
            if (t.status == "queued") {
                t.status = "sending";
                log(tr("JS8 TX status [%1]: sending").arg(t.label));
            }
            break;
        }
    } else if (type == "TX.COMPLETE") {
        // The sent text may lack our "CALL:" prefix and gain extras ("@G SNR" goes out as
        // "@G SNR -31"), so compare without the prefix and let either start the other.
        QString sent = bare(ev.value("value").toString());
        for (int i = 0; i < m_tx.size(); ++i) {
            QString want = bare(m_tx[i].expect);
            if (!sent.isEmpty() && !want.isEmpty() && (sent.startsWith(want) || want.startsWith(sent))) {
                PendingTx t = m_tx.takeAt(i);
                log(tr("JS8 TX status [%1]: sent").arg(t.label));
                reply(t.sender, tr("Transmitted on JS8: %1").arg(t.label));
                return;
            }
        }
        log(tr("JS8 TX.COMPLETE for something the gateway didn't send: '%1'").arg(ev.value("value").toString()));
    }
}

void Gateway::onDirected(const QJsonObject &ev) {
    QJsonObject p = ev.value("params").toObject();
    QString from = p.value("FROM").toString().trimmed().toUpper();
    QString to = p.value("TO").toString().trimmed().toUpper();
    while (to.endsWith('>')) to.chop(1);
    if (m_callsign.isEmpty()) return;
    bool groupOk = to.startsWith('@') && to != "@HB" &&
                   (m_s.groupsMode == Settings::GroupsAll ||
                    (m_s.groupsMode == Settings::GroupsList && m_s.groups.contains(to, Qt::CaseInsensitive)));
    if (to != m_callsign && !groupOk) return;

    // TEXT is "FROM: TO body"; keep the body, drop JS8's end-of-message mark and other non-ASCII
    QString text = p.value("TEXT").toString();
    if (text.isEmpty()) text = ev.value("value").toString();
    text = ascii(text);
    if (text.startsWith(from + ":")) text = text.mid(from.size() + 1).trimmed();
    static const QRegularExpression lead(QStringLiteral("^\\S+>*\\s*"));
    if (text.toUpper().startsWith(to)) text = text.remove(lead);

    QString key = from + '|' + to + '|' + text;
    QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_recent.begin(); it != m_recent.end();)
        it = it.value().secsTo(now) > m_s.dedupeSeconds ? m_recent.erase(it) : std::next(it);
    if (m_recent.contains(key)) return;
    m_recent.insert(key, now);

    QString snr;
    if (p.contains("SNR") && p.value("SNR").isDouble()) snr = QString::asprintf(" %+ddB", p.value("SNR").toInt());
    QString line = QStringLiteral("JS8 %1>%2%3: %4").arg(from, to, snr, text);
    log(tr("JS8 RX: %1").arg(line));
    for (const QString &node : m_s.allowedNodes) meshSend(node, line);
}

// ---- transmit tracking -------------------------------------------------------------------
QString Gateway::bare(const QString &text) const {
    QString t = text.trimmed().toUpper();
    if (!m_callsign.isEmpty() && t.startsWith(m_callsign + ":")) t = t.mid(m_callsign.size() + 1);
    static const QRegularExpression notCallChars(QStringLiteral("[^A-Z0-9@/]"));
    return t.remove(notCallChars);
}

void Gateway::trackTx(const QString &sender, const QString &label, const QString &expect) {
    m_tx.append({sender, label, expect, "queued", QDateTime::currentDateTimeUtc().addSecs(m_s.txReportSeconds)});
    log(tr("JS8 TX status [%1]: queued").arg(label));
}

void Gateway::checkTx() {
    QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = m_tx.size() - 1; i >= 0; --i) {
        if (m_tx[i].deadline > now) continue;
        PendingTx t = m_tx.takeAt(i);
        reply(t.sender, tr("JS8 hasn't transmitted after %1 min (status %2): %3")
                            .arg(m_s.txReportSeconds / 60).arg(t.status, t.label));
    }
}

void Gateway::withTxWarning(std::function<void(const QString &)> then) {
    request("TX.GET_ENABLED", {}, {}, [then](const QJsonObject &r, const QString &err) {
        bool off = err.isEmpty() && !r.value("params").toObject().value("ENABLED").toBool(true);
        then(off ? QStringLiteral(" WARNING: TX is OFF at the station.") : QString());
    });
}

void Gateway::sendDirected(const QString &sender, const QString &dest, const QString &body, const QString &label) {
    auto queued = [this, sender, label](const QString &expect) {
        log(tr("JS8 TX queued: %1").arg(label));
        trackTx(sender, label, expect);
        withTxWarning([this, sender, label](const QString &warn) {
            reply(sender, tr("Queued for JS8 TX: %1%2").arg(label, warn));
        });
    };
    request("TX.SEND_DIRECTED", {}, {{"TO", dest}, {"CMD", body}, {"PRIORITY", "NORMAL"}},
            [=](const QJsonObject &r, const QString &err) {
                QJsonObject p = r.value("params").toObject();
                if (!err.isEmpty() || !p.value("OK").toBool()) {
                    QString why = err.isEmpty() ? p.value("ERROR").toString() : err;
                    log(tr("JS8 TX failed: %1").arg(why));
                    reply(sender, tr("JS8 send failed: %1").arg(why));
                    return;
                }
                queued(p.value("COMPOSED").toString());
            });
}

void Gateway::waitForSelection(const QString &want, int tries, std::function<void(bool, const QString &)> done) {
    request("RX.GET_CALL_SELECTED", {}, {}, [=](const QJsonObject &r, const QString &err) {
        QString got = r.value("value").toString().trimmed().toUpper();
        if (err.isEmpty() && got == want) {
            done(true, got);
        } else if (tries <= 1) {
            done(false, err.isEmpty() ? got : err);
        } else {
            // the selection can show a moment after the reply
            QTimer::singleShot(500, this, [=]() { waitForSelection(want, tries - 1, done); });
        }
    });
}

void Gateway::selectAndThen(const QString &sender, const QString &call, std::function<void()> then) {
    request("RX.SET_CALL_SELECTED", call, {}, [=](const QJsonObject &, const QString &err) {
        if (!err.isEmpty()) {
            reply(sender, tr("Couldn't select %1: %2").arg(call.isEmpty() ? "none" : call, err));
            return;
        }
        waitForSelection(call, 5, [=](bool ok, const QString &got) {
            log(tr("JS8 selected call set: %1 -> %2").arg(call.isEmpty() ? "(none)" : call,
                                                          got.isEmpty() ? "(none)" : got));
            if (!ok) {
                reply(sender, tr("Asked the station to select %1, but it reports: %2")
                                  .arg(call.isEmpty() ? "none" : call, got.isEmpty() ? "none" : got));
                return;
            }
            then();
        });
    });
}

void Gateway::press(const QString &sender, const QString &action, const QString &expect, const QString &label) {
    request("UI.ACTION", action, {}, [=](const QJsonObject &r, const QString &err) {
        QJsonObject p = r.value("params").toObject();
        if (!err.isEmpty() || !p.value("OK").toBool()) {
            QString reason = err;
            if (reason.isEmpty()) {
                QString code = p.value("REASON").toString();
                reason = code == "disabled" ? tr("the button is unavailable now")
                       : code == "queued"   ? tr("a message is already waiting to send")
                                            : code;
            }
            log(tr("JS8 %1 failed: %2").arg(label, reason));
            reply(sender, tr("%1 failed: %2").arg(label, reason));
            return;
        }
        log(tr("JS8 button pressed: %1").arg(label));
        trackTx(sender, label, expect);
        withTxWarning([=](const QString &warn) { reply(sender, tr("%1 pressed.%2").arg(label, warn)); });
    });
}

// ---- commands ------------------------------------------------------------------------------
void Gateway::handleCommand(const QString &sender, const QString &text) {
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QStringList words = text.trimmed().split(ws, Qt::SkipEmptyParts);
    QString cmd = words.isEmpty() ? QString() : words[0].toUpper();
    while (cmd.startsWith('.')) cmd.remove(0, 1); // a leading "." is optional
    cmd = kAliases.value(cmd, cmd);
    QStringList args = words.mid(1);
    for (QString &a : args) a = a.toUpper();

    if (cmd == "HELP") {
        reply(sender, tr("JS8 gateway (short forms in caps, leading . optional): "
                         "SEnd [CALL|@GROUP] <msg> | INbox | HEard | STatus | FReq [MHz] | OFfset [Hz] | "
                         "GRoups [@G ..|ALL|NONE] | CAll [CALL|NONE] | CQ | REply [CALL] | SNr [CALL] | AGain | "
                         "HB | PIng | H/?"));

    } else if (cmd == "PING") {
        bool up = m_api && m_api->isConnected() && !m_callsign.isEmpty();
        QString where = m_api ? m_api->describe() : tr("JS8 station");
        reply(sender, up ? tr("Gateway up. %1 connected as %2.").arg(where, m_callsign)
                         : tr("Gateway up. %1 NOT connected.").arg(where));

    } else if (cmd == "SEND") {
        const QString usage = tr("Usage: SEND [CALL|@GROUP] <message> (no CALL: the selected call, see CA)");
        if (args.size() >= 2 && isCall(args[0])) {
            QString body = restAfter(text, 2).toUpper();
            sendDirected(sender, args[0], body, args[0] + ' ' + body);
        } else if (args.size() == 1 && isCall(args[0])) {
            reply(sender, tr("No message for %1. %2").arg(args[0], usage));
        } else if (!args.isEmpty()) {
            QString body = restAfter(text, 1).toUpper();
            // no destination given: send to the call or @group selected at the station
            request("RX.GET_CALL_SELECTED", {}, {}, [=](const QJsonObject &r, const QString &err) {
                QString dest = r.value("value").toString().trimmed().toUpper();
                if (!err.isEmpty()) reply(sender, tr("Couldn't read the selected call: %1").arg(err));
                else if (dest.isEmpty()) reply(sender, tr("No call selected. %1").arg(usage));
                else if (!isCall(dest)) reply(sender, tr("Selected call '%1' doesn't look like a callsign or @group.").arg(dest));
                else sendDirected(sender, dest, body, dest + ' ' + body);
            });
        } else {
            reply(sender, usage);
        }

    } else if (cmd == "INBOX") {
        cmdInbox(sender);

    } else if (cmd == "HEARD") {
        cmdHeard(sender);

    } else if (cmd == "STATUS") {
        cmdStatus(sender);

    } else if (cmd == "FREQ") {
        auto show = [=]() {
            request("RIG.GET_FREQ", {}, {}, [=](const QJsonObject &r, const QString &err) {
                if (!err.isEmpty()) { reply(sender, tr("FREQ failed: %1").arg(err)); return; }
                qint64 hz = r.value("params").toObject().value("DIAL").toVariant().toLongLong();
                QString band = bandOf(hz);
                reply(sender, tr("Dial %1 MHz (%2)").arg(hz / 1e6, 0, 'f', 3).arg(band.isEmpty() ? "out of band" : band));
            });
        };
        if (args.isEmpty()) { show(); return; }
        bool ok = false;
        double mhz = args[0].toDouble(&ok);
        if (!ok) { reply(sender, tr("Usage: FREQ  or  FREQ <MHz>, e.g. FREQ 14.078")); return; }
        qint64 hz = qRound64(mhz * 1e6);
        if (bandOf(hz).isEmpty()) { reply(sender, tr("%1 MHz is outside the ham bands; not changed.").arg(args[0])); return; }
        request("RIG.SET_FREQ", {}, {{"DIAL", hz}}, [=](const QJsonObject &, const QString &err) {
            if (!err.isEmpty()) { reply(sender, tr("FREQ failed: %1").arg(err)); return; }
            log(tr("JS8 dial set to %1").arg(hz));
            show();
        }, false);

    } else if (cmd == "OFFSET") {
        auto show = [=]() {
            request("RIG.GET_FREQ", {}, {}, [=](const QJsonObject &r, const QString &err) {
                if (!err.isEmpty()) { reply(sender, tr("OFFSET failed: %1").arg(err)); return; }
                reply(sender, tr("Offset %1 Hz").arg(r.value("params").toObject().value("OFFSET").toInt()));
            });
        };
        if (args.isEmpty()) { show(); return; }
        if (args[0] == "AUTO") { reply(sender, tr("Auto-offset isn't available in this gateway. Set an offset: OF <Hz>.")); return; }
        bool ok = false;
        int hz = args[0].toInt(&ok);
        if (!ok) { reply(sender, tr("Usage: OF  |  OF <Hz>, e.g. OF 1500")); return; }
        if (hz < kOffsetMin || hz > kOffsetMax) {
            reply(sender, tr("Offset must be %1-%2 Hz; not changed.").arg(kOffsetMin).arg(kOffsetMax));
            return;
        }
        request("RIG.SET_FREQ", {}, {{"OFFSET", hz}}, [=](const QJsonObject &, const QString &err) {
            if (!err.isEmpty()) { reply(sender, tr("OFFSET failed: %1").arg(err)); return; }
            log(tr("JS8 offset set to %1").arg(hz));
            show();
        }, false);

    } else if (cmd == "HB") {
        QString grid = m_grid.left(4);
        request("TX.SEND_DIRECTED", {}, {{"TO", "@HB"}, {"CMD", ("HEARTBEAT " + grid).trimmed()}, {"PRIORITY", "NORMAL"}},
                [=](const QJsonObject &r, const QString &err) {
                    QJsonObject p = r.value("params").toObject();
                    if (!err.isEmpty() || !p.value("OK").toBool()) {
                        reply(sender, tr("HB failed: %1").arg(err.isEmpty() ? p.value("ERROR").toString() : err));
                        return;
                    }
                    trackTx(sender, "heartbeat", p.value("COMPOSED").toString());
                    withTxWarning([=](const QString &w) { reply(sender, tr("Heartbeat queued for JS8 TX.%1").arg(w)); });
                });

    } else if (cmd == "GROUPS") {
        if (args.isEmpty()) { reply(sender, m_s.groupsText()); return; }
        if (args == QStringList{"ALL"}) {
            m_s.groupsMode = Settings::GroupsAll;
        } else if (args == QStringList{"NONE"}) {
            m_s.groupsMode = Settings::GroupsNone;
        } else {
            QStringList groups, bad;
            for (QString g : args) {
                if (!g.startsWith('@')) g.prepend('@');
                if (!kGroupRe.match(g).hasMatch() || g == "@HB") bad << g;
                else if (!groups.contains(g)) groups << g;
            }
            if (!bad.isEmpty()) {
                reply(sender, tr("Not a group name: %1. Usage: GR | GR @G1 @G2 | GR ALL | GR NONE").arg(bad.join(' ')));
                return;
            }
            m_s.groupsMode = Settings::GroupsList;
            m_s.groups = groups;
        }
        m_s.save();
        emit settingsChanged(m_s);
        log(tr("Forward groups set: %1").arg(m_s.groupsText()));
        reply(sender, m_s.groupsText());

    } else if (cmd == "CALL") {
        auto show = [=]() {
            request("RX.GET_CALL_SELECTED", {}, {}, [=](const QJsonObject &r, const QString &err) {
                QString got = r.value("value").toString().trimmed().toUpper();
                if (!err.isEmpty()) reply(sender, tr("CALL failed: %1").arg(err));
                else reply(sender, got.isEmpty() ? tr("No call selected.") : tr("Selected call: %1").arg(got));
            });
        };
        if (args.isEmpty()) { show(); return; }
        QString want = args[0];
        if (want == "NONE" || want == "-") want.clear();
        else if (!isCall(want)) { reply(sender, tr("'%1' doesn't look like a callsign or @group.").arg(want)); return; }
        selectAndThen(sender, want, show);

    } else if (cmd == "CQ" || cmd == "REPLY" || cmd == "SNR") {
        if (cmd == "CQ") {
            // CQ goes to no one: clear any selection first
            request("RX.GET_CALL_SELECTED", {}, {}, [=](const QJsonObject &r, const QString &err) {
                if (!err.isEmpty()) { reply(sender, tr("CQ failed: %1").arg(err)); return; }
                if (r.value("value").toString().trimmed().isEmpty()) press(sender, "cq", "CQ", "CQ");
                else selectAndThen(sender, QString(), [=]() { press(sender, "cq", "CQ", "CQ"); });
            });
            return;
        }
        auto go = [=](const QString &call) {
            if (call.startsWith('@')) { reply(sender, tr("%1 needs a station, not a group (%2 is selected).").arg(cmd, call)); return; }
            press(sender, cmd.toLower(), cmd == "SNR" ? call + " SNR" : call, tr("%1 to %2").arg(cmd, call));
        };
        if (!args.isEmpty()) {
            QString call = args[0];
            if (!isCall(call) || call.startsWith('@')) { reply(sender, tr("'%1' doesn't look like a callsign.").arg(call)); return; }
            selectAndThen(sender, call, [=]() { go(call); });
        } else {
            request("RX.GET_CALL_SELECTED", {}, {}, [=](const QJsonObject &r, const QString &err) {
                QString call = r.value("value").toString().trimmed().toUpper();
                if (!err.isEmpty()) reply(sender, tr("%1 failed: %2").arg(cmd, err));
                else if (call.isEmpty()) reply(sender, tr("No call selected. Usage: %1 <CALL>, or select one with CA.").arg(cmd));
                else go(call);
            });
        }

    } else if (cmd == "AGAIN") {
        // the phone's Repeat: put the last sent message back in the box, then press Send
        request("UI.ACTION", "repeat", {}, [=](const QJsonObject &r, const QString &err) {
            QJsonObject p = r.value("params").toObject();
            if (!err.isEmpty() || !p.value("OK").toBool()) {
                QString why = !err.isEmpty() ? err
                            : tr("nothing sent yet, or a message is already waiting to send");
                log(tr("JS8 AGAIN failed: %1").arg(why));
                reply(sender, tr("AGAIN failed: %1").arg(why));
                return;
            }
            request("TX.GET_TEXT", {}, {}, [=](const QJsonObject &t, const QString &e) {
                QString text = ascii(t.value("value").toString()).trimmed();
                if (!e.isEmpty() || text.isEmpty()) {
                    reply(sender, tr("AGAIN failed: %1").arg(e.isEmpty() ? tr("the station's message box is empty") : e));
                    return;
                }
                QString label = text;
                if (label.toUpper().startsWith(m_callsign + ":")) label = label.mid(m_callsign.size() + 1).trimmed();
                press(sender, "send", text, tr("AGAIN: %1").arg(label));
            });
        });

    } else {
        reply(sender, tr("Unknown command. Send HELP."));
    }
}

void Gateway::cmdInbox(const QString &sender) {
    request("INBOX.GET_MESSAGES", {}, {}, [=](const QJsonObject &r, const QString &err) {
        if (!err.isEmpty()) { reply(sender, tr("Inbox read failed: %1").arg(err)); return; }
        struct Item { QDateTime t; QString line; };
        QList<Item> items;
        for (const auto &v : r.value("params").toObject().value("MESSAGES").toArray()) {
            QJsonObject m = v.toObject(), p = m.value("params").toObject();
            QJsonValue utc = p.value("UTC");
            QDateTime t;
            if (utc.isDouble()) {
                t = QDateTime::fromMSecsSinceEpoch(qint64(utc.toDouble())).toUTC();
            } else { // the inbox stores UTC as text
                QDateTime local = QDateTime::fromString(utc.toString(), "yyyy-MM-dd HH:mm:ss");
                t = QDateTime::fromMSecsSinceEpoch(0).toUTC().addDays(QDate(1970, 1, 1).daysTo(local.date()))
                        .addMSecs(local.time().msecsSinceStartOfDay());
            }
            QString unread = m.value("type").toString().compare("UNREAD", Qt::CaseInsensitive) == 0 ? "*" : "";
            QString body = p.value("TEXT").toString().trimmed();
            if (body.isEmpty()) body = m.value("value").toString();
            items.append({t, QStringLiteral("%1%2 UTC %3>%4: %5").arg(unread, t.toString("HH:mm:ss"),
                                                                      p.value("FROM").toString("?"), p.value("TO").toString("?"), ascii(body))});
        }
        if (items.isEmpty()) { reply(sender, tr("JS8 inbox is empty.")); return; }
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.t < b.t; });
        items = items.mid(std::max(0, int(items.size()) - m_s.inboxCount));
        reply(sender, tr("JS8 inbox, latest %1 (* = unread):").arg(items.size()));
        for (const Item &i : items) reply(sender, i.line);
    });
}

void Gateway::cmdHeard(const QString &sender) {
    request("RX.GET_CALL_ACTIVITY", {}, {}, [=](const QJsonObject &r, const QString &err) {
        if (!err.isEmpty()) { reply(sender, tr("HEARD failed: %1").arg(err)); return; }
        struct Spot { QString call, grid; qint64 utc; QJsonValue snr; };
        QList<Spot> spots;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        QJsonObject p = r.value("params").toObject();
        for (auto it = p.begin(); it != p.end(); ++it) {
            if (it.key().startsWith('_') || !it.value().isObject() || it.key() == m_callsign) continue;
            QJsonObject d = it.value().toObject();
            qint64 utc = qint64(d.value("UTC").toDouble());
            if (now - utc > qint64(m_s.heardSeconds) * 1000) continue;
            spots.append({it.key(), d.value("GRID").toString().trimmed(), utc, d.value("SNR")});
        }
        if (spots.isEmpty()) { reply(sender, tr("No stations heard in the last %1 min.").arg(m_s.heardSeconds / 60)); return; }
        std::sort(spots.begin(), spots.end(), [](const Spot &a, const Spot &b) { return a.utc > b.utc; });
        QStringList parts;
        for (const Spot &s : spots.mid(0, m_s.heardCount)) {
            QString snr = s.snr.isDouble() ? QString::asprintf(" %+d", s.snr.toInt()) : QString();
            QString grid = s.grid.isEmpty() ? QString() : ' ' + s.grid;
            parts << QStringLiteral("%1%2%3 %4").arg(s.call, snr, grid, ago(s.utc));
        }
        reply(sender, tr("Heard (%1): %2").arg(spots.size()).arg(parts.join(", ")));
    });
}

void Gateway::cmdStatus(const QString &sender) {
    request("RIG.GET_FREQ", {}, {}, [=](const QJsonObject &f, const QString &err) {
        if (!err.isEmpty()) { reply(sender, tr("STATUS failed: %1").arg(err)); return; }
        QJsonObject fp = f.value("params").toObject();
        qint64 dial = fp.value("DIAL").toVariant().toLongLong();
        int offset = fp.value("OFFSET").toInt();
        auto finish = [=](const QString &speed) {
            request("RX.GET_CALL_ACTIVITY", {}, {}, [=](const QJsonObject &a, const QString &) {
                qint64 last = 0;
                QJsonObject ap = a.value("params").toObject();
                for (auto it = ap.begin(); it != ap.end(); ++it)
                    if (it.value().isObject() && it.key() != m_callsign)
                        last = std::max(last, qint64(it.value().toObject().value("UTC").toDouble()));
                reply(sender, tr("%1 dial %2 MHz offset %3 Hz, speed %4, last RX %5")
                                  .arg(m_callsign).arg(dial / 1e6, 0, 'f', 3).arg(offset).arg(speed,
                                       last ? ago(last) + tr(" ago") : tr("none yet")));
            });
        };
        request("MODE.GET_SUBMODE_NAME", {}, {}, [=](const QJsonObject &s, const QString &e) {
            finish(e.isEmpty() ? s.value("params").toObject().value("NAME").toString().toLower() : "?");
        });
    });
}
