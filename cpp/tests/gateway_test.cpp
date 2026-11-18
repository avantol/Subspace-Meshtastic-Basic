// The gateway's commands and forwarding against a fake Subspace station.
#include "Gateway.h"
#include "Js8Api.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QStandardPaths>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what, const QString &got = {}) {
    if (!ok) {
        std::printf("FAIL: %s\n      got: %s\n", what, qPrintable(got));
        ++failures;
    }
}

// Answers like a Subspace station whose state the test sets.
class FakeApi : public Js8Api {
  public:
    QString selected = "@MAGTG";
    QString lastTx, box; // the last message sent, and the station's message box
    QStringList calls; // "TYPE value params" of each request
    QJsonObject activity, inbox;
    void start() override {}
    QString describe() const override { return "Fake"; }
    bool isConnected() const override { return true; }
    void push(const QJsonObject &ev) { emit event(ev); }
    void up() { emit connectedChanged(true); }
    void request(const QString &type, const QString &value, const QVariantMap &params, Callback cb, bool) override {
        QStringList ps;
        for (auto it = params.begin(); it != params.end(); ++it) ps << it.key() + "=" + it.value().toString();
        calls << QString("%1 %2 %3").arg(type, value, ps.join(',')).trimmed();
        auto r = [](const QString &v, const QJsonObject &p = {}) { return QJsonObject{{"value", v}, {"params", p}}; };
        if (type == "STATION.GET_CALLSIGN") cb(r("WM8Q"), {});
        else if (type == "STATION.GET_GRID") cb(r("DN61OL"), {});
        else if (type == "RX.GET_CALL_SELECTED") cb(r(selected), {});
        else if (type == "RX.SET_CALL_SELECTED") { selected = value; cb(r(""), {}); } // reply value is empty, as Subspace's
        else if (type == "TX.SEND_DIRECTED")
            cb(r("", {{"OK", true}, {"COMPOSED", "WM8Q: " + params["TO"].toString() + " " + params["CMD"].toString()}}), {});
        else if (type == "TX.GET_ENABLED") cb(r("", {{"ENABLED", true}}), {});
        else if (type == "UI.ACTION" && value == "repeat") {
            bool ok = !lastTx.isEmpty();
            if (ok) box = lastTx;
            cb(r(value, {{"OK", ok}, {"REASON", ok ? "" : "disabled"}}), {});
        } else if (type == "TX.GET_TEXT") cb(r(box), {});
        else if (type == "UI.ACTION" && value == "send") {
            bool ok = !box.isEmpty();
            cb(r(value, {{"OK", ok}, {"REASON", ok ? "" : "disabled"}}), {});
        } else if (type == "UI.ACTION") {
            bool ok = !(value != "cq" && selected.startsWith('@'));
            cb(r(value, {{"OK", ok}, {"REASON", ok ? "" : "disabled"}}), {});
        } else if (type == "RIG.GET_FREQ") cb(r("", {{"DIAL", 7078000}, {"OFFSET", 1500}}), {});
        else if (type == "RIG.SET_FREQ") cb({}, {});
        else if (type == "MODE.GET_SUBMODE_NAME") cb(r("", {{"NAME", "Normal"}}), {});
        else if (type == "RX.GET_CALL_ACTIVITY") cb(r("", activity), {});
        else if (type == "INBOX.GET_MESSAGES") cb(r("", inbox), {});
        else cb({}, "unknown " + type);
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true); // GROUPS saves settings: keep them out of the real config
    QCoreApplication::setOrganizationName("js8mesh-test");

    Gateway gw;
    FakeApi api;
    QStringList out;
    QObject::connect(&gw, &Gateway::meshQueued, [&](const QString &, const QString &t) { out << t; });
    Settings s;
    s.allowedNodes = {"!8a31cb4a"};
    gw.setApiForTest(&api, s);
    api.up();

    auto run = [&](const QString &cmd) {
        out.clear();
        api.calls.clear();
        gw.handleCommand("!8a31cb4a", cmd);
        return out.join(" | ");
    };
    auto rx = [&](const QString &from, const QString &to, const QString &text) {
        out.clear();
        api.push({{"type", "RX.DIRECTED"}, {"value", from + ": " + to + " " + text},
                  {"params", QJsonObject{{"FROM", from}, {"TO", to}, {"TEXT", from + ": " + to + " " + text + " ♢ "}, {"SNR", -7}}}});
        return out.join(" | ");
    };
    QString r;

    r = run("pi");
    check(r == "Gateway up. Fake connected as WM8Q.", "PING", r);

    r = run("se k1abc hello there");
    check(r == "Queued for JS8 TX: K1ABC HELLO THERE", "SEND to a call", r);
    check(api.calls.value(0) == "TX.SEND_DIRECTED  CMD=HELLO THERE,PRIORITY=NORMAL,TO=K1ABC", "SEND request", api.calls.value(0));
    out.clear();
    api.push({{"type", "TX.COMPLETE"}, {"value", "WM8Q: K1ABC HELLO THERE ♢"}});
    check(out.join("") == "Transmitted on JS8: K1ABC HELLO THERE", "TX.COMPLETE confirms", out.join(""));

    r = run("se 73");
    check(r == "Queued for JS8 TX: @MAGTG 73", "SEND to the selected call", r);
    r = run("se K1ABC");
    check(r.startsWith("No message for K1ABC."), "SEND with only a call", r);

    r = run("sn");
    check(r == "SNR needs a station, not a group (@MAGTG is selected).", "SNR with a group selected", r);
    r = run("cq");
    check(r == "CQ pressed." && api.selected.isEmpty(), "CQ clears the selection", r);
    r = run("sn kd8skz");
    check(r == "SNR to KD8SKZ pressed." && api.selected == "KD8SKZ", "SNR selects the call", r);
    out.clear();
    api.push({{"type", "TX.COMPLETE"}, {"value", "KD8SKZ SNR -31 "}});
    check(out.join("") == "Transmitted on JS8: SNR to KD8SKZ", "TX.COMPLETE without our prefix", out.join(""));
    r = run("ag");
    check(r == "AGAIN failed: nothing sent yet, or a message is already waiting to send", "AGAIN with nothing sent", r);
    api.lastTx = "WM8Q: KD8SKZ SNR -31";
    r = run("again");
    check(r == "AGAIN: KD8SKZ SNR -31 pressed.", "AGAIN restores and sends", r);
    check(api.calls.join(";") == "UI.ACTION repeat;TX.GET_TEXT;UI.ACTION send;TX.GET_ENABLED", "AGAIN requests", api.calls.join(";"));
    out.clear();
    api.push({{"type", "TX.COMPLETE"}, {"value", "KD8SKZ SNR -31 "}});
    check(out.join("") == "Transmitted on JS8: AGAIN: KD8SKZ SNR -31", "AGAIN confirmed", out.join(""));
    r = run("ca");
    check(r == "Selected call: KD8SKZ", "CALL shows", r);
    r = run("ca @subspace");
    check(r == "Selected call: @SUBSPACE", "CALL sets despite an empty reply", r);

    r = run("fr 7.078");
    check(r == "Dial 7.078 MHz (40m)" && api.calls.value(0) == "RIG.SET_FREQ  DIAL=7078000", "FREQ set", r);
    r = run("fr 6.5");
    check(r == "6.5 MHz is outside the ham bands; not changed.", "FREQ out of band", r);
    r = run("of 5000");
    check(r == "Offset must be 300-2700 Hz; not changed.", "OFFSET range", r);
    r = run("of");
    check(r == "Offset 1500 Hz", "OFFSET show", r);

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    api.activity = {{"_ID", 5}, {"KI6QDB", QJsonObject{{"SNR", 0}, {"GRID", "DM12"}, {"UTC", double(now - 5 * 60000)}}},
                    {"KK7URE", QJsonObject{{"SNR", -4}, {"GRID", "DN08"}, {"UTC", double(now - 44 * 60000)}}},
                    {"OLD1", QJsonObject{{"SNR", 1}, {"GRID", ""}, {"UTC", double(now - 3 * 3600000)}}}};
    r = run("he");
    check(r == "Heard (2): KI6QDB +0 DM12 5m, KK7URE -4 DN08 44m", "HEARD", r);
    r = run("st");
    check(r == "WM8Q dial 7.078 MHz offset 1500 Hz, speed normal, last RX 5m ago", "STATUS", r);

    auto msg = [](const char *type, const char *utc, const char *from, const char *text) {
        return QJsonObject{{"type", type}, {"value", text},
                           {"params", QJsonObject{{"UTC", utc}, {"FROM", from}, {"TO", "WM8Q"}, {"TEXT", text}}}};
    };
    api.inbox = {{"MESSAGES", QJsonArray{msg("READ", "2026-11-15 19:01:26", "W0IFM", "HELLO"),
                                         msg("UNREAD", "2026-11-16 03:08:11", "KJ7ROX", "73")}}};
    r = run("in");
    check(r == "JS8 inbox, latest 2 (* = unread): | 19:01:26 UTC W0IFM>WM8Q: HELLO | *03:08:11 UTC KJ7ROX>WM8Q: 73", "INBOX", r);

    r = run("gr");
    check(r == "Forwarding no @groups (only messages to you).", "GROUPS default none", r);
    r = rx("K1ABC", "@MAGNET", "HI ALL");
    check(r.isEmpty(), "group not forwarded by default", r);
    r = run("gr @magnet allcall");
    check(r == "Forwarding @groups: @ALLCALL @MAGNET", "GROUPS set", r);
    r = rx("K1ABC", "@MAGNET", "HI ALL");
    check(r == "JS8 K1ABC>@MAGNET -7dB: HI ALL", "chosen group forwarded", r);
    r = rx("K1ABC", "@MAGNET", "HI ALL");
    check(r.isEmpty(), "repeat dropped", r);
    r = rx("K1ABC", "@OTHER", "HI");
    check(r.isEmpty(), "other group not forwarded", r);
    r = rx("K1ABC", "WM8Q", "SNR?");
    check(r == "JS8 K1ABC>WM8Q -7dB: SNR?", "message to us forwarded", r);
    r = run("gr @hb");
    check(r.startsWith("Not a group name: @HB."), "@HB refused", r);
    r = run("gr none");

    r = run("bogus");
    check(r == "Unknown command. Send HELP.", "unknown", r);
    r = run(".h");
    check(r.startsWith("(1/2) JS8 gateway (short forms") && r.contains("| SNr [CALL]") && out.size() == 2,
          "HELP with a leading dot, split at a space", r);

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
