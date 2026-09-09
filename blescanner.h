#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>

/*
 * BLE discovery with an optional name-prefix filter.
 *
 * Only devices passing the filter are appended to m_found, so a list
 * widget row index maps directly onto results().
 */
class BleScanner : public QObject
{
    Q_OBJECT

public:
    explicit BleScanner(QObject *parent = nullptr);

    // e.g. "Electroporator" - matches Electroporator01, Electroporator02, ...
    void setNamePrefix(const QString &prefix) { m_prefix = prefix; }

    const QList<QBluetoothDeviceInfo> &results() const { return m_found; }

public slots:
    void start();
    void stop();

signals:
    void deviceFound(const QString &name, const QString &address, int rssi);
    void status(const QString &msg);
    void finished();

private:
    void requestPermissionThenScan();
    void beginScan();
    void onDeviceDiscovered(const QBluetoothDeviceInfo &info);
    void onError(QBluetoothDeviceDiscoveryAgent::Error e);

    QBluetoothDeviceDiscoveryAgent *m_agent = nullptr;
    QList<QBluetoothDeviceInfo>     m_found;
    QString                         m_prefix;
};
