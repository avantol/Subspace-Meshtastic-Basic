#pragma once
// Meshtastic over Bluetooth LE (the node's GATT service): write ToRadio, read FromRadio until
// it comes back empty, and read again whenever the FromNum characteristic notifies.
#include "MeshLink.h"

#include <QBluetoothDeviceInfo>
#include <QLowEnergyCharacteristic>
#include <QPointer>
#include <QQueue>

class QBluetoothDeviceDiscoveryAgent;
class QLowEnergyController;
class QLowEnergyService;

class BleMeshLink : public MeshLink {
    Q_OBJECT
  public:
    // device: the node's Bluetooth name (e.g. "Rpt1_be91") or address (e.g. "FD:08:0B:7B:BE:91")
    explicit BleMeshLink(const QString &device, QObject *parent = nullptr);
    ~BleMeshLink() override;
    void start() override;
    QString describe() const override;
    QString address() const { return m_address; } // known once the node has been found

  protected:
    void writeToRadio(const QByteArray &toRadio) override;

  private:
    void onDeviceFound(const QBluetoothDeviceInfo &info);
    void connectTo(const QBluetoothDeviceInfo &info);
    void onServiceState();
    void readFromRadio();
    void writeNext();

    QString m_device, m_address;
    QBluetoothDeviceDiscoveryAgent *m_agent = nullptr;
    QLowEnergyController *m_ctl = nullptr;
    QPointer<QLowEnergyService> m_svc;
    QLowEnergyCharacteristic m_toRadio, m_fromRadio, m_fromNum;
    QQueue<QByteArray> m_writes;
    bool m_writing = false, m_reading = false, m_readAgain = false, m_found = false;
    int m_emptyReads = 0;
};
