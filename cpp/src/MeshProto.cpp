#include "MeshProto.h"

namespace meshproto {
namespace {

enum WireType { Varint = 0, Fixed64 = 1, LengthDelimited = 2, Fixed32 = 5 };

void putVarint(QByteArray &b, quint64 v) {
    while (v >= 0x80) {
        b.append(char((v & 0x7f) | 0x80));
        v >>= 7;
    }
    b.append(char(v));
}
void putTag(QByteArray &b, int field, WireType wt) { putVarint(b, (quint64(field) << 3) | wt); }
void putVarintField(QByteArray &b, int field, quint64 v) {
    putTag(b, field, Varint);
    putVarint(b, v);
}
void putFixed32Field(QByteArray &b, int field, quint32 v) {
    putTag(b, field, Fixed32);
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xff));
}
void putBytesField(QByteArray &b, int field, const QByteArray &v) {
    putTag(b, field, LengthDelimited);
    putVarint(b, quint64(v.size()));
    b.append(v);
}

struct Reader {
    const uchar *p, *end;
    bool ok = true;
    explicit Reader(const QByteArray &b)
        : p(reinterpret_cast<const uchar *>(b.constData())), p0(p), end(p + b.size()) {}
    const uchar *p0;
    bool atEnd() const { return !ok || p >= end; }
    quint64 varint() {
        quint64 v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) { ok = false; return 0; }
            uchar c = *p++;
            v |= quint64(c & 0x7f) << shift;
            if (!(c & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    quint32 fixed32() {
        if (end - p < 4) { ok = false; return 0; }
        quint32 v = quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24;
        p += 4;
        return v;
    }
    QByteArray bytes() {
        quint64 n = varint();
        if (!ok || quint64(end - p) < n) { ok = false; return {}; }
        QByteArray v(reinterpret_cast<const char *>(p), int(n));
        p += n;
        return v;
    }
    void skip(int wt) {
        switch (wt) {
        case Varint: varint(); break;
        case Fixed64: if (end - p < 8) ok = false; else p += 8; break;
        case LengthDelimited: bytes(); break;
        case Fixed32: fixed32(); break;
        default: ok = false;
        }
    }
};

void parseData(const QByteArray &b, FromRadio &out) {
    Reader r(b);
    while (!r.atEnd()) {
        quint64 tag = r.varint();
        int field = int(tag >> 3), wt = int(tag & 7);
        if (field == 1 && wt == Varint) out.portnum = int(r.varint());
        else if (field == 2 && wt == LengthDelimited) out.payload = r.bytes();
        else r.skip(wt);
    }
}

void parsePacket(const QByteArray &b, FromRadio &out) {
    Reader r(b);
    while (!r.atEnd()) {
        quint64 tag = r.varint();
        int field = int(tag >> 3), wt = int(tag & 7);
        if (field == 1 && wt == Fixed32) out.from = r.fixed32();
        else if (field == 2 && wt == Fixed32) out.to = r.fixed32();
        else if (field == 3 && wt == Varint) out.channel = quint32(r.varint());
        else if (field == 4 && wt == LengthDelimited) { out.decoded = true; parseData(r.bytes(), out); }
        else if (field == 6 && wt == Fixed32) out.packetId = r.fixed32();
        else r.skip(wt);
    }
}

void parseUser(const QByteArray &b, FromRadio &out) {
    Reader r(b);
    while (!r.atEnd()) {
        quint64 tag = r.varint();
        int field = int(tag >> 3), wt = int(tag & 7);
        if (field == 1 && wt == LengthDelimited) out.userId = QString::fromUtf8(r.bytes());
        else if (field == 2 && wt == LengthDelimited) out.longName = QString::fromUtf8(r.bytes());
        else if (field == 3 && wt == LengthDelimited) out.shortName = QString::fromUtf8(r.bytes());
        else r.skip(wt);
    }
}

void parseNodeInfo(const QByteArray &b, FromRadio &out) {
    Reader r(b);
    while (!r.atEnd()) {
        quint64 tag = r.varint();
        int field = int(tag >> 3), wt = int(tag & 7);
        if (field == 1 && wt == Varint) out.nodeNum = quint32(r.varint());
        else if (field == 2 && wt == LengthDelimited) parseUser(r.bytes(), out);
        else r.skip(wt);
    }
}

quint32 parseMyNodeNum(const QByteArray &b) {
    Reader r(b);
    while (!r.atEnd()) {
        quint64 tag = r.varint();
        int field = int(tag >> 3), wt = int(tag & 7);
        if (field == 1 && wt == Varint) return quint32(r.varint());
        r.skip(wt);
    }
    return 0;
}

} // namespace

QByteArray toRadioWantConfig(quint32 configId) {
    QByteArray b;
    putVarintField(b, 3, configId); // ToRadio.want_config_id
    return b;
}

QByteArray toRadioHeartbeat() {
    QByteArray b;
    putBytesField(b, 7, QByteArray()); // ToRadio.heartbeat (empty Heartbeat)
    return b;
}

QByteArray toRadioText(quint32 to, quint32 packetId, const QByteArray &utf8, int hopLimit,
                       bool wantAck, quint32 channel) {
    QByteArray data;
    putVarintField(data, 1, kTextMessageApp); // Data.portnum
    putBytesField(data, 2, utf8);             // Data.payload

    QByteArray pkt;
    putFixed32Field(pkt, 2, to);              // MeshPacket.to
    if (channel) putVarintField(pkt, 3, channel);
    putBytesField(pkt, 4, data);              // MeshPacket.decoded
    putFixed32Field(pkt, 6, packetId);        // MeshPacket.id
    if (hopLimit > 0) putVarintField(pkt, 9, quint64(hopLimit));
    if (wantAck) putVarintField(pkt, 10, 1);

    QByteArray b;
    putBytesField(b, 1, pkt);                 // ToRadio.packet
    return b;
}

bool parseFromRadio(const QByteArray &bytes, FromRadio &out) {
    out = FromRadio();
    Reader r(bytes);
    while (!r.atEnd()) {
        quint64 tag = r.varint();
        int field = int(tag >> 3), wt = int(tag & 7);
        if (field == 2 && wt == LengthDelimited) { out.kind = FromRadio::Packet; parsePacket(r.bytes(), out); }
        else if (field == 3 && wt == LengthDelimited) { out.kind = FromRadio::MyInfo; out.myNodeNum = parseMyNodeNum(r.bytes()); }
        else if (field == 4 && wt == LengthDelimited) { out.kind = FromRadio::NodeInfo; parseNodeInfo(r.bytes(), out); }
        else if (field == 7 && wt == Varint) { out.kind = FromRadio::ConfigComplete; out.configId = quint32(r.varint()); }
        else if (field == 8 && wt == Varint) { r.varint(); out.kind = FromRadio::Rebooted; }
        else r.skip(wt);
    }
    return r.ok;
}

bool parseNodeId(const QString &id, quint32 &num) {
    QString s = id.trimmed();
    if (!s.startsWith(QLatin1Char('!')) || s.size() != 9) return false;
    bool ok = false;
    num = s.mid(1).toUInt(&ok, 16);
    return ok;
}

} // namespace meshproto
