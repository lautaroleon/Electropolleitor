#include "blelink.h"
#include <QDebug>

// Braces matter: QBluetoothUuid's QString ctor expects the canonical form.
const QBluetoothUuid BleLink::ServiceUuid(
    QStringLiteral("{6E400001-B5A3-F393-E0A9-E50E24DCCA9E}"));
const QBluetoothUuid BleLink::RxUuid(
    QStringLiteral("{6E400002-B5A3-F393-E0A9-E50E24DCCA9E}"));
const QBluetoothUuid BleLink::TxUuid(
    QStringLiteral("{6E400003-B5A3-F393-E0A9-E50E24DCCA9E}"));

BleLink::BleLink(QObject *parent)
    : QObject(parent)
{
}

void BleLink::cleanup()
{
    m_ready = false;
    m_rxChar = QLowEnergyCharacteristic();
    m_rxBuf.clear();                    // <-- here
    if (m_svc)  { m_svc->deleteLater();  m_svc  = nullptr; }
    if (m_ctrl) { m_ctrl->deleteLater(); m_ctrl = nullptr; }
}

void BleLink::connectTo(const QBluetoothDeviceInfo &info)
{
    cleanup();

    emit status(QStringLiteral("connecting to %1...").arg(info.name()));

    m_ctrl = QLowEnergyController::createCentral(info, this);

    connect(m_ctrl, &QLowEnergyController::connected,
            this, &BleLink::onConnected);
    connect(m_ctrl, &QLowEnergyController::disconnected,
            this, &BleLink::onDisconnected);
    connect(m_ctrl, &QLowEnergyController::serviceDiscovered,
            this, &BleLink::onServiceDiscovered);
    connect(m_ctrl, &QLowEnergyController::discoveryFinished,
            this, &BleLink::onDiscoveryFinished);
    connect(m_ctrl, &QLowEnergyController::errorOccurred,
            this, [this](QLowEnergyController::Error e) {
                emit status(QStringLiteral("controller error %1: %2")
                            .arg(int(e)).arg(m_ctrl->errorString()));
            });

    m_ctrl->connectToDevice();
}

void BleLink::disconnectFromDevice()
{
    if (m_ctrl) m_ctrl->disconnectFromDevice();
}

void BleLink::onConnected()
{
    emit status(QStringLiteral("connected - discovering services"));
    m_ctrl->discoverServices();          // NOT automatic
}

void BleLink::onDisconnected()
{
    emit status(QStringLiteral("disconnected"));
    cleanup();
    emit disconnected();
}

void BleLink::onServiceDiscovered(const QBluetoothUuid &uuid)
{
    qDebug() << "[ble] service" << uuid.toString();
}

void BleLink::onDiscoveryFinished()
{
    m_svc = m_ctrl->createServiceObject(ServiceUuid, this);

    if (!m_svc) {
        emit status(QStringLiteral("Nordic UART Service NOT found"));
        return;
    }

    connect(m_svc, &QLowEnergyService::stateChanged,
            this, &BleLink::onServiceStateChanged);
    connect(m_svc, &QLowEnergyService::characteristicChanged,
            this, &BleLink::onCharacteristicChanged);
    connect(m_svc, &QLowEnergyService::descriptorWritten,
            this, &BleLink::onDescriptorWritten);
    connect(m_svc, &QLowEnergyService::errorOccurred,
            this, [this](QLowEnergyService::ServiceError e) {
                emit status(QStringLiteral("service error %1").arg(int(e)));
            });

    // Characteristics are NOT populated until this completes.
    emit status(QStringLiteral("found NUS - discovering details"));
    m_svc->discoverDetails();
}

void BleLink::onServiceStateChanged(QLowEnergyService::ServiceState s)
{
    if (s != QLowEnergyService::RemoteServiceDiscovered)
        return;

    m_rxChar = m_svc->characteristic(RxUuid);
    const QLowEnergyCharacteristic txChar = m_svc->characteristic(TxUuid);

    if (!m_rxChar.isValid() || !txChar.isValid()) {
        emit status(QStringLiteral("RX/TX characteristic missing"));
        return;
    }

    // Enable notifications by writing 0x0100 to the CCCD (descriptor 0x2902).
    const QLowEnergyDescriptor cccd = txChar.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);

    if (!cccd.isValid()) {
        emit status(QStringLiteral("CCCD missing - is BLE2902 in the sketch?"));
        return;
    }

    emit status(QStringLiteral("enabling notifications"));
    m_svc->writeDescriptor(cccd, QByteArray::fromHex("0100"));
    // ready() is deliberately NOT emitted here. The header promises that
    // ready() means notifications are actually enabled, so it waits for
    // descriptorWritten - otherwise the first replies can be dropped.
}

void BleLink::onDescriptorWritten(const QLowEnergyDescriptor &d,
                                  const QByteArray &value)
{
    if (d.type() != QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration)
        return;
    if (value != QByteArray::fromHex("0100"))
        return;                          // a disable, or some other write
    if (m_ready)
        return;                          // treat as at-least-once, not once

    m_ready = true;
    emit status(QStringLiteral("ready"));
    emit ready();
}

void BleLink::onCharacteristicChanged(const QLowEnergyCharacteristic &c,
                                      const QByteArray &value)
{
    if (c.uuid() != TxUuid) return;

    m_rxBuf.append(value);
    int nl;
    while ((nl = m_rxBuf.indexOf('\n')) >= 0) {
        QByteArray line = m_rxBuf.left(nl);
        m_rxBuf.remove(0, nl + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty()) emit lineReceived(QString::fromUtf8(line));
    }
    if (m_rxBuf.size() > 512) m_rxBuf.clear();
}
void BleLink::send(const QString &line)
{
    if (!m_ready || !m_svc || !m_rxChar.isValid()) {
        emit status(QStringLiteral("send ignored - link not ready"));
        return;
    }

    // Default ATT MTU is 23 bytes => 20 bytes of payload. Longer writes
    // are silently truncated or rejected until the MTU is negotiated up.
    QByteArray payload = line.toUtf8();
    if (!payload.endsWith('\n')) payload.append('\n');
    m_svc->writeCharacteristic(m_rxChar, payload, QLowEnergyService::WriteWithResponse);
    //cleanup();
    //m_rxBuf.clear();
}
