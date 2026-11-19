#include "BleMeshLink.h"
#include "QtCompat.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QLowEnergyService>
#include <QTimer>

namespace {
// Meshtastic's GATT service (meshtastic/python ble_interface.py)
const QBluetoothUuid kService(QStringLiteral("6ba1b218-15a8-461f-9fa8-5dcae273eafd"));
const QBluetoothUuid kToRadio(QStringLiteral("f75c76d2-129e-4dad-a1dd-7866124401e7"));
const QBluetoothUuid kFromRadio(QStringLiteral("2c55e69e-4993-11ed-b878-0242ac120002"));
const QBluetoothUuid kFromNum(QStringLiteral("ed9da18c-a800-4f66-a670-aa7547e34453"));
constexpr int kScanMs = 30 * 1000;
constexpr int kSetupMs = 60 * 1000;
constexpr int kEmptyReadRetries = 5; // a reply can lag a write a little; read again a few times
}

BleMeshLink::BleMeshLink(const QString &device, const QString &knownAddress, QObject *parent)
    : MeshLink(parent), m_device(cleanName(device)), m_knownAddress(knownAddress.trimmed()) {}

QString BleMeshLink::cleanName(const QString &name) {
    QString s = name;
    int nul = s.indexOf(QChar(0));
    if (nul >= 0) s.truncate(nul);
    QString out;
    for (QChar c : s)
        if (c.isPrint()) out += c;
    return out.trimmed();
}

bool BleMeshLink::matches(const QString &setting, const QString &knownAddress, const QString &name,
                          const QString &address) {
    QString want = cleanName(setting), got = cleanName(name);
    if (want.isEmpty()) return false;
    if (address.compare(want, Qt::CaseInsensitive) == 0) return true;
    if (!knownAddress.isEmpty() && address.compare(knownAddress, Qt::CaseInsensitive) == 0 &&
        address != QLatin1String("00:00:00:00:00:00"))
        return true;
    if (got.isEmpty()) return false;
    if (got.compare(want, Qt::CaseInsensitive) == 0) return true;
    // one name cut short or padded: "Rpt1_be91" vs "Rpt1_be91<junk>"
    const QString &shorter = got.size() < want.size() ? got : want;
    const QString &longer = got.size() < want.size() ? want : got;
    return shorter.size() >= 4 && longer.startsWith(shorter, Qt::CaseInsensitive);
}

BleMeshLink::~BleMeshLink() {
    if (m_ctl) m_ctl->disconnectFromDevice();
}

QString BleMeshLink::describe() const { return tr("Bluetooth %1").arg(m_device); }

void BleMeshLink::start() {
    m_agent = new QBluetoothDeviceDiscoveryAgent(this);
    m_agent->setLowEnergyDiscoveryTimeout(kScanMs);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this, &BleMeshLink::onDeviceFound);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceUpdated, this,
            [this](const QBluetoothDeviceInfo &info, QBluetoothDeviceInfo::Fields) { onDeviceFound(info); });
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished, this, [this]() {
        if (!m_found)
            fail(tr("Bluetooth device '%1' not found (out of range, or another device is connected to it?). Seen: %2")
                     .arg(m_device, m_seen.isEmpty() ? tr("nothing") : m_seen.join(", ")));
    });
    connect(m_agent, JS8_AGENT_ERROR, this,
            [this]() { fail(tr("Bluetooth scan failed: %1").arg(m_agent->errorString())); });
    emit logMessage(tr("Scanning for Bluetooth device %1...").arg(m_device));
    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void BleMeshLink::onDeviceFound(const QBluetoothDeviceInfo &info) {
    if (m_found || m_failed) return;
    QString addr = info.address().toString();
    bool match = matches(m_device, m_knownAddress, info.name(), addr) ||
                 info.deviceUuid().toString(QUuid::WithoutBraces).compare(m_device, Qt::CaseInsensitive) == 0;
    if (!match) {
        QString seen = QStringLiteral("%1 (%2)").arg(cleanName(info.name()), addr);
        if (!m_seen.contains(seen) && m_seen.size() < 20) m_seen << seen;
        return;
    }
    m_found = true;
    m_agent->stop();
    connectTo(info);
}

void BleMeshLink::connectTo(const QBluetoothDeviceInfo &info) {
    m_address = info.address().toString();
    emit logMessage(tr("Found %1 (%2, %3 dBm); connecting...").arg(cleanName(info.name()), m_address).arg(info.rssi()));
    m_ctl = QLowEnergyController::createCentral(info, this);
    // connecting and discovering the service must finish in time; a weak link can stall there
    QTimer::singleShot(kSetupMs, this, [this]() {
        if (!m_toRadio.isValid()) fail(tr("Bluetooth connection timed out (weak signal?)"));
    });
    connect(m_ctl, &QLowEnergyController::connected, this, [this]() { m_ctl->discoverServices(); });
    connect(m_ctl, &QLowEnergyController::disconnected, this, [this]() { fail(tr("Bluetooth link lost")); });
    connect(m_ctl, JS8_CTL_ERROR, this,
            [this]() { fail(tr("Bluetooth error: %1").arg(m_ctl->errorString())); });
    connect(m_ctl, &QLowEnergyController::discoveryFinished, this, [this]() {
        m_svc = m_ctl->createServiceObject(kService, this);
        if (!m_svc) {
            fail(tr("this Bluetooth device has no Meshtastic service"));
            return;
        }
        connect(m_svc, &QLowEnergyService::stateChanged, this, &BleMeshLink::onServiceState);
        connect(m_svc, JS8_SVC_ERROR, this,
                [this](QLowEnergyService::ServiceError e) { onServiceError(int(e)); });
        connect(m_svc, &QLowEnergyService::characteristicChanged, this,
                [this](const QLowEnergyCharacteristic &c, const QByteArray &) {
                    if (c.uuid() == kFromNum) { m_emptyReads = 0; readFromRadio(); }
                });
        connect(m_svc, &QLowEnergyService::characteristicRead, this,
                [this](const QLowEnergyCharacteristic &c, const QByteArray &value) {
                    if (c.uuid() != kFromRadio) return;
                    m_reading = false;
                    if (!value.isEmpty()) {
                        m_emptyReads = 0;
                        handleFromRadio(value);
                        readFromRadio();
                    } else if (m_readAgain) {
                        m_readAgain = false;
                        readFromRadio();
                    } else if (m_emptyReads < kEmptyReadRetries) {
                        ++m_emptyReads;
                        QTimer::singleShot(100, this, [this]() { readFromRadio(); });
                    }
                });
        connect(m_svc, &QLowEnergyService::characteristicWritten, this,
                [this](const QLowEnergyCharacteristic &c, const QByteArray &) {
                    if (c.uuid() != kToRadio) return;
                    m_writing = false;
                    m_emptyReads = 0;
                    readFromRadio(); // the node may have an answer
                    writeNext();
                });
        connect(m_svc, &QLowEnergyService::descriptorWritten, this,
                [this](const QLowEnergyDescriptor &, const QByteArray &) {
                    if (!m_cccdPending) return;
                    m_cccdPending = false;
                    // notifications can still go missing (seen on Windows): check every few seconds too
                    startPolling(3000);
                    transportOpened();
                });
        m_svc->discoverDetails();
    });
    m_ctl->connectToDevice();
}

void BleMeshLink::onServiceState() {
    if (m_svc->state() != JS8_SVC_DISCOVERED) return;
    m_toRadio = m_svc->characteristic(kToRadio);
    m_fromRadio = m_svc->characteristic(kFromRadio);
    m_fromNum = m_svc->characteristic(kFromNum);
    if (!m_toRadio.isValid() || !m_fromRadio.isValid() || !m_fromNum.isValid()) {
        fail(tr("the node's Bluetooth service is missing a characteristic"));
        return;
    }
    // notifications on FromNum tell us when there is something to read
    QLowEnergyDescriptor cccd =
        m_fromNum.descriptor(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    if (cccd.isValid()) {
        m_cccdPending = true;
        m_svc->writeDescriptor(cccd, QByteArray::fromHex("0100"));
    }
    else {
        startPolling(1000);
        transportOpened();
    }
}

void BleMeshLink::startPolling(int ms) {
    if (!m_poll) {
        m_poll = new QTimer(this);
        connect(m_poll, &QTimer::timeout, this, [this]() {
            if (m_reading) return;
            m_emptyReads = kEmptyReadRetries; // one read; no retries when it comes back empty
            readFromRadio();
        });
    }
    m_poll->start(ms);
}

void BleMeshLink::onServiceError(int e) {
    if (e == QLowEnergyService::DescriptorWriteError && m_cccdPending) {
        // notifications refused (Windows does this for some links): ask the node every second
        m_cccdPending = false;
        emit logMessage(tr("Bluetooth: the node's notifications couldn't be turned on; polling it instead."));
        startPolling(1000);
        transportOpened();
        return;
    }
    QString what;
    switch (e) {
    case QLowEnergyService::CharacteristicReadError: what = tr("reading from the node failed"); break;
    case QLowEnergyService::CharacteristicWriteError: what = tr("writing to the node failed"); break;
    case QLowEnergyService::DescriptorWriteError: what = tr("turning on the node's notifications failed"); break;
    case QLowEnergyService::DescriptorReadError: what = tr("reading a descriptor failed"); break;
    default: what = tr("service error %1").arg(e);
    }
    bool access = e == QLowEnergyService::CharacteristicReadError || e == QLowEnergyService::CharacteristicWriteError ||
                  e == QLowEnergyService::DescriptorWriteError;
    // a failed read/write is fatal for the link (reconnect)
    fail(access ? tr("Bluetooth: %1. The node probably needs pairing: pair it with this computer once "
                     "(Windows: Settings > Bluetooth & devices > Add device; Linux: bluetoothctl pair) "
                     "using the node's Bluetooth PIN, then start again.").arg(what)
                : tr("Bluetooth: %1").arg(what));
}

void BleMeshLink::writeToRadio(const QByteArray &toRadio) {
    m_writes.enqueue(toRadio);
    writeNext();
}

void BleMeshLink::writeNext() {
    if (m_writing || m_writes.isEmpty() || !m_svc || !m_toRadio.isValid()) return;
    m_writing = true;
    m_svc->writeCharacteristic(m_toRadio, m_writes.dequeue(), QLowEnergyService::WriteWithResponse);
}

void BleMeshLink::readFromRadio() {
    if (!m_svc || !m_fromRadio.isValid() || m_failed) return;
    if (m_reading) {
        m_readAgain = true;
        return;
    }
    m_reading = true;
    m_svc->readCharacteristic(m_fromRadio);
}
