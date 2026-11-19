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

BleMeshLink::BleMeshLink(const QString &device, QObject *parent) : MeshLink(parent), m_device(device.trimmed()) {}

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
        if (!m_found) fail(tr("Bluetooth device '%1' not found (out of range, or another device is connected to it?)").arg(m_device));
    });
    connect(m_agent, JS8_AGENT_ERROR, this,
            [this]() { fail(tr("Bluetooth scan failed: %1").arg(m_agent->errorString())); });
    emit logMessage(tr("Scanning for Bluetooth device %1...").arg(m_device));
    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void BleMeshLink::onDeviceFound(const QBluetoothDeviceInfo &info) {
    if (m_found || m_failed) return;
    bool match = info.name().compare(m_device, Qt::CaseInsensitive) == 0 ||
                 info.address().toString().compare(m_device, Qt::CaseInsensitive) == 0 ||
                 info.deviceUuid().toString(QUuid::WithoutBraces).compare(m_device, Qt::CaseInsensitive) == 0;
    if (!match) return;
    m_found = true;
    m_agent->stop();
    connectTo(info);
}

void BleMeshLink::connectTo(const QBluetoothDeviceInfo &info) {
    m_address = info.address().toString();
    emit logMessage(tr("Found %1 (%2, %3 dBm); connecting...").arg(info.name(), m_address).arg(info.rssi()));
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
                [this](QLowEnergyService::ServiceError e) {
                    // a failed read/write is reported here too; treat all as fatal (reconnect)
                    fail(tr("Bluetooth service error %1").arg(int(e)));
                });
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
                [this](const QLowEnergyDescriptor &, const QByteArray &) { transportOpened(); });
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
    if (cccd.isValid())
        m_svc->writeDescriptor(cccd, QByteArray::fromHex("0100"));
    else
        transportOpened();
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
