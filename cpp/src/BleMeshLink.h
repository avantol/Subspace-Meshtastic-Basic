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
class QTimer;

class BleMeshLink : public MeshLink {
    Q_OBJECT
  public:
    // device: the node's Bluetooth name (e.g. "Rpt1_be91") or address (e.g. "FD:08:0B:7B:BE:91");
    // knownAddress: the address the scan dialog saw for it, also accepted (may be empty)
    explicit BleMeshLink(const QString &device, const QString &knownAddress = {}, QObject *parent = nullptr);
    ~BleMeshLink() override;
    void start() override;
    QString describe() const override;
    QString address() const { return m_address; } // known once the node has been found
    // A device name without what Windows can attach to it (a NUL and junk after it, control
    // characters, spaces at the ends).
    static QString cleanName(const QString &name);
    // Does a scanned device with this name and address match the setting?
    static bool matches(const QString &setting, const QString &knownAddress, const QString &name,
                        const QString &address);

  protected:
    void writeToRadio(const QByteArray &toRadio) override;

  private:
    void onDeviceFound(const QBluetoothDeviceInfo &info);
    void connectTo(const QBluetoothDeviceInfo &info);
    void onServiceState();
    void readFromRadio();
    void writeNext();
    void onServiceError(int error);
    void startPolling(int ms);

    QString m_device, m_knownAddress, m_address;
    QStringList m_seen; // names seen during the scan, for the not-found message
    QBluetoothDeviceDiscoveryAgent *m_agent = nullptr;
    QLowEnergyController *m_ctl = nullptr;
    QPointer<QLowEnergyService> m_svc;
    QLowEnergyCharacteristic m_toRadio, m_fromRadio, m_fromNum;
    QQueue<QByteArray> m_writes;
    bool m_writing = false, m_reading = false, m_readAgain = false, m_found = false;
    int m_emptyReads = 0;
    bool m_cccdPending = false; // waiting for the notifications switch to be written
    QTimer *m_poll = nullptr;   // reads FromRadio every second when notifications are off
};
