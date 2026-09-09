#pragma once

#include <QObject>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QLowEnergyCharacteristic>

/*
 * Step 2: connect to a peripheral, find the Nordic UART Service,
 * subscribe to notifications, send lines.
 *
 * Emits ready() only when the link is genuinely usable - service
 * discovered AND notifications enabled. Do not send before that.
 */
class BleLink : public QObject
{
    Q_OBJECT

public:
    static const QBluetoothUuid ServiceUuid;
    static const QBluetoothUuid RxUuid;   // app -> device, write
    static const QBluetoothUuid TxUuid;   // device -> app, notify

    explicit BleLink(QObject *parent = nullptr);

    bool isReady() const { return m_ready; }

public slots:
    void connectTo(const QBluetoothDeviceInfo &info);
    void disconnectFromDevice();
    void send(const QString &line);

signals:
    void status(const QString &msg);
    void ready();
    void lineReceived(const QString &line);
    void disconnected();

private:
    void onConnected();
    void onDisconnected();
    void onServiceDiscovered(const QBluetoothUuid &uuid);
    void onDiscoveryFinished();
    void onServiceStateChanged(QLowEnergyService::ServiceState s);
    void onCharacteristicChanged(const QLowEnergyCharacteristic &c,
                                 const QByteArray &value);
    void onDescriptorWritten(const QLowEnergyDescriptor &d,
                             const QByteArray &value);
    void cleanup();

    QLowEnergyController     *m_ctrl = nullptr;
    QLowEnergyService        *m_svc  = nullptr;
    QLowEnergyCharacteristic  m_rxChar;
    bool                      m_ready = false;

    QByteArray m_rxBuf;
};
