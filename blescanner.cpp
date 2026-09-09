#include "blescanner.h"

#include <QCoreApplication>
#include <QPermissions>
#include <QDebug>

BleScanner::BleScanner(QObject *parent)
    : QObject(parent)
{
    m_agent = new QBluetoothDeviceDiscoveryAgent(this);
    m_agent->setLowEnergyDiscoveryTimeout(10000);   // ms; 0 = until stop()

    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
            this, &BleScanner::onDeviceDiscovered);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
            this, &BleScanner::onError);
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished,
            this, [this] { emit finished(); });
    connect(m_agent, &QBluetoothDeviceDiscoveryAgent::canceled,
            this, [this] { emit finished(); });
}

void BleScanner::start()
{
    if (m_agent->isActive())
        m_agent->stop();

    m_found.clear();
    requestPermissionThenScan();
}

void BleScanner::stop()
{
    if (m_agent->isActive())
        m_agent->stop();
}

void BleScanner::requestPermissionThenScan()
{
#if QT_CONFIG(permissions)
    QBluetoothPermission perm;
    perm.setCommunicationModes(QBluetoothPermission::Access);

    switch (qApp->checkPermission(perm)) {
    case Qt::PermissionStatus::Undetermined:
        emit status(QStringLiteral("requesting bluetooth permission"));
        // Async. The scan MUST start from inside this callback.
        qApp->requestPermission(perm, this, [this](const QPermission &p) {
            if (p.status() == Qt::PermissionStatus::Granted)
                beginScan();
            else
                emit status(QStringLiteral("bluetooth permission DENIED"));
        });
        return;

    case Qt::PermissionStatus::Denied:
        emit status(QStringLiteral("bluetooth permission DENIED "
                                   "- check AndroidManifest.xml"));
        return;

    case Qt::PermissionStatus::Granted:
        break;
    }
#endif
    beginScan();
}

void BleScanner::beginScan()
{
    emit status(QStringLiteral("scanning..."));

    // LowEnergyMethod is essential - the default also runs Classic
    // discovery, which cannot see a BLE-only peripheral.
    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void BleScanner::onDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
    if (!(info.coreConfigurations()
          & QBluetoothDeviceInfo::LowEnergyCoreConfiguration)) {
        return;   // Classic-only device
    }

    // Filter here so m_found stays index-aligned with the UI list.
    if (!m_prefix.isEmpty()
        && !info.name().startsWith(m_prefix, Qt::CaseInsensitive)) {
        return;
    }

    // Android re-reports the same device on every advertising packet.
    for (const QBluetoothDeviceInfo &seen : m_found) {
        if (seen.address() == info.address()
            && seen.deviceUuid() == info.deviceUuid()) {
            return;
        }
    }

    m_found.append(info);

    // Android gives a MAC; iOS/macOS give an opaque UUID instead.
    const QString addr = info.address().isNull()
                       ? info.deviceUuid().toString()
                       : info.address().toString();

    qDebug() << "[ble]" << info.name() << addr << "rssi" << info.rssi();
    emit deviceFound(info.name(), addr, info.rssi());
}

void BleScanner::onError(QBluetoothDeviceDiscoveryAgent::Error e)
{
    emit status(QStringLiteral("error %1: %2")
                .arg(int(e)).arg(m_agent->errorString()));
}
