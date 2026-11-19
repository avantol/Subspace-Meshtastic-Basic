// Checks MeshProto against bytes produced by the official Meshtastic Python protobufs.
#include "BleMeshLink.h"
#include "MeshProto.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } \
    } while (0)

static QByteArray hex(const char *h) { return QByteArray::fromHex(h); }

int main() {
    using namespace meshproto;
    CHECK(toRadioText(0x8a31cb4a, 0x12345678, "hi", 3, true) ==
          hex("0a16154acb318a2206080112026869357856341248035001"));
    CHECK(toRadioWantConfig(42) == hex("182a"));
    CHECK(toRadioHeartbeat() == hex("3a00"));

    FromRadio f;
    CHECK(parseFromRadio(hex("080712320d4acb318a1591be7b0b220a08011206706920e29c9335efbeadde3d40591b"
                             "69450000c840480360a6ffffffffffffffff01"), f));
    CHECK(f.kind == FromRadio::Packet);
    CHECK(f.from == 0x8a31cb4a && f.to == 0x0b7bbe91 && f.packetId == 0xdeadbeef);
    CHECK(f.decoded && f.portnum == kTextMessageApp);
    CHECK(QString::fromUtf8(f.payload) == QString::fromUtf8("pi ✓"));

    CHECK(parseFromRadio(hex("08011a070891fdee5b4005"), f));
    CHECK(f.kind == FromRadio::MyInfo && f.myNodeNum == 0x0b7bbe91);

    CHECK(parseFromRadio(hex("0802222c08ca96c7d108121a0a09213861333163623461120757696f204f6e651a0457"
                             "696f3125000060402d63000000"), f));
    CHECK(f.kind == FromRadio::NodeInfo && f.nodeNum == 0x8a31cb4a);
    CHECK(f.userId == "!8a31cb4a" && f.longName == "Wio One" && f.shortName == "Wio1");

    CHECK(parseFromRadio(hex("0803382a"), f));
    CHECK(f.kind == FromRadio::ConfigComplete && f.configId == 42);

    CHECK(parseFromRadio(hex("120e0d0500000015ffffffff2a020102"), f));
    CHECK(f.kind == FromRadio::Packet && !f.decoded && f.from == 5);

    CHECK(!parseFromRadio(hex("12ff"), f)); // truncated

    quint32 n = 0;
    CHECK(parseNodeId("!0b7bbe91", n) && n == 0x0b7bbe91);
    CHECK(nodeId(0x0b7bbe91) == "!0b7bbe91");
    CHECK(!parseNodeId("0b7bbe91", n));

    // Bluetooth names as Windows can report them
    CHECK(BleMeshLink::cleanName(QString("Rpt1_be91") + QChar(0) + "\x01xyz") == "Rpt1_be91");
    CHECK(BleMeshLink::matches("Rpt1_be91", "", "Rpt1_be91", "FD:08:0B:7B:BE:91"));
    CHECK(BleMeshLink::matches("rpt1_be91", "", "Rpt1_be91\x03\x19junk", "FD:08:0B:7B:BE:91"));
    CHECK(BleMeshLink::matches("Rpt1_be91garbage", "", "Rpt1_be91", "FD:08:0B:7B:BE:91"));
    CHECK(BleMeshLink::matches("Something", "FD:08:0B:7B:BE:91", "", "fd:08:0b:7b:be:91"));
    CHECK(BleMeshLink::matches("FD:08:0B:7B:BE:91", "", "", "FD:08:0B:7B:BE:91"));
    CHECK(!BleMeshLink::matches("Rpt1_be91", "", "Rpt2_9e66", "AA:BB:CC:DD:EE:FF"));
    CHECK(!BleMeshLink::matches("Rpt1_be91", "", "Rpt", "AA:BB:CC:DD:EE:FF"));

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
