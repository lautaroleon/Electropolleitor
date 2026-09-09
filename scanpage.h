#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class ScanPage; }
QT_END_NAMESPACE

/*
 * Full-screen device picker page.
 *
 * Exposes a small façade rather than its widgets, so MainWindow never
 * reaches into ui->listDevices and the two forms stay independent.
 */
class ScanPage : public QWidget
{
    Q_OBJECT

public:
    explicit ScanPage(QWidget *parent = nullptr);
    ~ScanPage();

    void reset();                            // clear list, back to "scanning"
    void addDevice(const QString &text);
    void setTitle(const QString &text);
    void setBusy(bool busy);                 // true while connecting
    int  deviceCount() const;

signals:
    void deviceChosen(int row);              // row indexes BleScanner::results()
    void rescanRequested();
    void cancelled();

private:
    Ui::ScanPage *ui;
};
