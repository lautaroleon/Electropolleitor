#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#pragma once
#include <QMainWindow>
#include <QTimer>
#include <QListWidgetItem>
#include <QKeyEvent>
#include <QDebug>
#include <QStackedWidget>
#include <QInputDialog>

class BleScanner;
class BleLink;
class ScanPage;
class QStackedWidget;

struct Params { int nPulses = 5; int widthMs = 30; int gapMs = 100; double volts = 10.0; };

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    /*
     * The app's view of the device. Every transition comes from a device
     * message or from a timeout - the app never assumes a state, and never
     * enables Activate on anything it inferred by itself.
     *
     * Armed is a UI hint computed from MV vs the acknowledged setpoint. The
     * firmware runs its own, faster settle check and remains the authority:
     * it can still answer NAK FIRE VOLT.
     */
    enum class State { Offline, Settling, Armed, Firing, Fault };

private slots:
    void on_btnConnect_clicked();
    void onDeviceFound(const QString &name, const QString &address, int rssi);
    void onStatus(const QString &msg);
    void onScanFinished();
    void onDeviceChosen(int row);
    void onLinkReady();
    void onLinkDisconnected();
    void onLineReceived(const QString &line);
    void onActivateClicked();
    void onAbortClicked();
    void on_npulses_clicked();
    void on_pulseduration_clicked();
    void on_pulsespacing_clicked();
    void on_setvolt_clicked();

private:
    void startScan();
    void showMainPage();
    void askAndSend(const QString &title, const QString &unit, const QString &key,
                    double cur, double lo, double hi, int decimals);
    void setControlsEnabled(bool up);

    void setState(State s);
    void updateStateLabel();
    void refreshArming();
    void onMeasured(double volts);
    void onMvStale();
    void onFireTimeout();
    void showOutcome(const QString &msg, const QString &colour = QString());
    void setStateColour(const QString &colour);

    Params m_p;
    Ui::MainWindow *ui;
    QStackedWidget *m_stack    = nullptr;
    QWidget        *m_mainPage = nullptr;
    ScanPage       *m_scanPage = nullptr;
    BleScanner     *m_scanner  = nullptr;
    BleLink        *m_link     = nullptr;

    State   m_state       = State::Offline;
    double  m_mv          = 0.0;
    int     m_inTolCount  = 0;
    bool    m_mvStale     = true;
    bool    m_outcomeHeld = false;   // holding a DONE/refused message visible
    QString m_stateColour;           // last colour pushed to lblState

    QTimer *m_mvWatchdog   = nullptr;
    QTimer *m_fireWatchdog = nullptr;
    QTimer *m_outcomeTimer = nullptr;

protected:
    void keyPressEvent(QKeyEvent *e) override;
};

#endif // MAINWINDOW_H
