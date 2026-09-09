#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "blescanner.h"
#include "blelink.h"
#include "scanpage.h"

namespace {

// The app's settle detector is a UI hint. MV arrives at 4 Hz, so two
// consecutive in-tolerance samples is ~500 ms - coarser than the firmware's
// own 50 Hz check by design. The firmware still decides whether FIRE runs.
constexpr double kTolAbs      = 0.3;    // volts
constexpr double kTolFrac     = 0.02;   // 2 % of setpoint
constexpr int    kInTolNeeded = 2;

// MV doubles as the liveness indicator (design notes S6). If it stops, the
// link is sick even without a disconnect - never leave Activate live on
// stale telemetry.
constexpr int    kMvStaleMs   = 1500;

// Worst-case train is 7 x 70 ms + 6 x 200 ms = 1690 ms. If no DONE arrives
// well past that, a message was dropped; recover rather than stick in Firing.
constexpr int    kFireTimeoutMs = 4000;

constexpr int    kOutcomeHoldMs = 2500;

// Instrument Light state colours. Kept next to the state machine rather than
// in the stylesheet because lblState's colour is a function of m_state, and
// Qt stylesheets cannot express that.
constexpr auto kColOffline  = "#5c6f7e";   // muted - nothing to act on
constexpr auto kColSettling = "#b45309";   // amber - not ready yet
constexpr auto kColArmed    = "#1c8a4d";   // green - Activate is live
constexpr auto kColFiring   = "#d97706";   // orange - delivering
constexpr auto kColFault    = "#c0392b";   // red - reserved for faults

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_scanner(new BleScanner(this))
{
    ui->setupUi(this);

    // The stylesheet colours the readouts, but only Flat and Filled segments
    // honour the foreground colour at all - the default Outline style draws
    // from the palette's light/dark roles and disappears on a pale window.
    for (QLCDNumber *lcd : { ui->lcdpulses, ui->lcdduration, ui->lcdspacing,
                             ui->lcdsetvolt, ui->readvolt })
        lcd->setSegmentStyle(QLCDNumber::Flat);

    // Wrap the existing form as page 0. takeCentralWidget() detaches it
    // without destroying it, so every ui-> pointer stays valid.
    m_mainPage = takeCentralWidget();
    m_scanPage = new ScanPage;

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_mainPage);
    m_stack->addWidget(m_scanPage);
    setCentralWidget(m_stack);
    m_stack->setCurrentWidget(m_mainPage);

    m_scanner->setNamePrefix(QStringLiteral("EPORATOR"));
    m_link = new BleLink(this);

    connect(m_scanner, &BleScanner::deviceFound, this, &MainWindow::onDeviceFound);
    connect(m_scanner, &BleScanner::status,      this, &MainWindow::onStatus);
    connect(m_scanner, &BleScanner::finished,    this, &MainWindow::onScanFinished);

    connect(m_link, &BleLink::status,       this, &MainWindow::onStatus);
    connect(m_link, &BleLink::lineReceived, this, &MainWindow::onLineReceived);
    connect(m_link, &BleLink::ready,        this, &MainWindow::onLinkReady);
    connect(m_link, &BleLink::disconnected, this, &MainWindow::onLinkDisconnected);

    connect(m_scanPage, &ScanPage::deviceChosen,    this, &MainWindow::onDeviceChosen);
    connect(m_scanPage, &ScanPage::rescanRequested, this, &MainWindow::startScan);
    connect(m_scanPage, &ScanPage::cancelled,       this, &MainWindow::showMainPage);

    // Explicit, not connectSlotsByName. The auto-connection wants
    // on_activate_clicked(); naming it anything else silently wires nothing,
    // which is exactly how this button spent its first life doing nothing.
    connect(ui->activate, &QPushButton::clicked, this, &MainWindow::onActivateClicked);
    connect(ui->abort,    &QPushButton::clicked, this, &MainWindow::onAbortClicked);

    m_mvWatchdog = new QTimer(this);
    m_mvWatchdog->setSingleShot(true);
    connect(m_mvWatchdog, &QTimer::timeout, this, &MainWindow::onMvStale);

    m_fireWatchdog = new QTimer(this);
    m_fireWatchdog->setSingleShot(true);
    connect(m_fireWatchdog, &QTimer::timeout, this, &MainWindow::onFireTimeout);

    m_outcomeTimer = new QTimer(this);
    m_outcomeTimer->setSingleShot(true);
    connect(m_outcomeTimer, &QTimer::timeout, this, [this] {
        m_outcomeHeld = false;
        updateStateLabel();
    });

    setControlsEnabled(false);
    m_state = State::Settling;          // force setState() to actually apply
    setState(State::Offline);
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::startScan()
{
    m_scanPage->reset();
    m_stack->setCurrentWidget(m_scanPage);
    m_scanner->start();
}

void MainWindow::showMainPage()
{
    m_scanner->stop();
    m_stack->setCurrentWidget(m_mainPage);
}

void MainWindow::on_btnConnect_clicked()
{
    qDebug() << "[ui] btnConnect clicked, ready=" << m_link->isReady();

    if (m_link->isReady()) {
        m_link->disconnectFromDevice();
        return;
    }
    startScan();
}

void MainWindow::onDeviceFound(const QString &name, const QString &addr, int rssi)
{
    m_scanPage->addDevice(QStringLiteral("%1\n%2   %3 dBm")
                              .arg(name, addr).arg(rssi));
}

void MainWindow::onScanFinished()
{
    m_scanPage->setBusy(false);
    const int n = m_scanPage->deviceCount();
    m_scanPage->setTitle(n == 0 ? tr("No electroporators found")
                                : tr("%1 found - tap to connect").arg(n));
}

void MainWindow::onDeviceChosen(int row)
{
    const auto &devices = m_scanner->results();
    if (row < 0 || row >= devices.size()) return;

    m_scanner->stop();
    m_scanPage->setBusy(true);
    m_scanPage->setTitle(tr("Connecting..."));
    m_link->connectTo(devices.at(row));

    QTimer::singleShot(12000, this, [this] {
        if (!m_link->isReady() && m_stack->currentWidget() == m_scanPage) {
            m_scanPage->setTitle(tr("Connection failed - rescan"));
            m_scanPage->setBusy(false);
        }
    });
}

// Parameter buttons only. Activate and Abort are governed by setState(),
// since being connected is not sufficient to fire.
void MainWindow::setControlsEnabled(bool up)
{
    ui->npulses->setEnabled(up);
    ui->pulseduration->setEnabled(up);
    ui->pulsespacing->setEnabled(up);
    ui->setvolt->setEnabled(up);
}

void MainWindow::onLinkReady()
{
    showMainPage();
    setControlsEnabled(true);
    ui->btnConnect->setText(tr("Disconnect"));

    m_inTolCount = 0;
    m_mvStale    = true;
    setState(State::Settling);

    // ready() now means the CCCD write completed, so this delay is only
    // belt-and-braces against Android's ordering quirks.
    QTimer::singleShot(100, this, [this]{ if (m_link->isReady()) m_link->send("GET ALL"); });
}

void MainWindow::onLinkDisconnected()
{
    setControlsEnabled(false);
    ui->btnConnect->setText(tr("Connect"));

    m_mvWatchdog->stop();
    m_fireWatchdog->stop();
    m_inTolCount = 0;
    m_mvStale    = true;
    setState(State::Offline);
}

void MainWindow::onStatus(const QString &msg)
{
    ui->lblStatus->setText(msg);
    qDebug() << "[status]" << msg;
}

// ---------------------------------------------------------------- state

void MainWindow::setState(State s)
{
    if (m_state == s) return;
    m_state = s;

    const char *pix = ":/normal.png";
    switch (s) {
    case State::Offline:  pix = ":/normal.png";      break;
    case State::Settling: pix = ":/normal.png";      break;
    case State::Armed:    pix = ":/happy.png";       break;
    case State::Firing:   pix = ":/electrocuted.png"; break;
    case State::Fault:    pix = ":/freaking.png";    break;
    }
    ui->pollo->setPixmap(QPixmap(pix));

    ui->activate->setEnabled(s == State::Armed);
    ui->abort->setEnabled(s == State::Firing);

    // A fault or a new train outranks any held result message. Without this,
    // FAULT arriving inside the hold window changes the picture but leaves
    // the previous "done - N pulses" text on screen.
    if (s == State::Fault || s == State::Firing) {
        m_outcomeHeld = false;
        m_outcomeTimer->stop();
    }

    updateStateLabel();
}

void MainWindow::updateStateLabel()
{
    if (m_outcomeHeld) return;          // let the last result stay readable

    QString text;
    QString colour;
    switch (m_state) {
    case State::Offline:
        text   = tr("not connected");
        colour = kColOffline;
        break;
    case State::Settling:
        text = m_mvStale ? tr("waiting for telemetry")
                         : tr("settling   %1 / %2 V")
                               .arg(m_mv, 0, 'f', 1).arg(m_p.volts, 0, 'f', 1);
        colour = kColSettling;
        break;
    case State::Armed:
        text   = tr("armed at %1 V").arg(m_p.volts, 0, 'f', 1);
        colour = kColArmed;
        break;
    case State::Firing:
        text   = tr("firing...");
        colour = kColFiring;
        break;
    case State::Fault:
        text   = tr("regulator fault - check the supply and sense divider");
        colour = kColFault;
        break;
    }
    ui->lblState->setText(text);
    setStateColour(colour);
}

// setStyleSheet re-polishes the widget, and updateStateLabel() runs on every
// MV sample - 4 Hz - while settling. Only touch the sheet on a real change.
void MainWindow::setStateColour(const QString &colour)
{
    if (colour.isEmpty() || colour == m_stateColour) return;
    m_stateColour = colour;
    ui->lblState->setStyleSheet(QStringLiteral("color: %1;").arg(colour));
}

void MainWindow::showOutcome(const QString &msg, const QString &colour)
{
    ui->lblState->setText(msg);
    setStateColour(colour);          // empty keeps whatever the state set
    m_outcomeHeld = true;
    m_outcomeTimer->start(kOutcomeHoldMs);
}

void MainWindow::refreshArming()
{
    // Firing and Fault are device-declared. A measurement does not clear them;
    // only DONE, a NAK, or a timeout does.
    if (m_state == State::Offline || m_state == State::Firing
        || m_state == State::Fault)
        return;

    const bool armed = !m_mvStale && m_inTolCount >= kInTolNeeded;
    setState(armed ? State::Armed : State::Settling);
}

void MainWindow::onMeasured(double volts)
{
    m_mv      = volts;
    m_mvStale = false;
    // Fixed one decimal on both voltage readouts. display(double) drops the
    // trailing zero, so a setpoint of 30.0 would render "30" beside a
    // measurement of "30.1" and read as two different precisions.
    ui->readvolt->display(QString::number(volts, 'f', 1));
    m_mvWatchdog->start(kMvStaleMs);

    const double tol = qMax(kTolAbs, kTolFrac * m_p.volts);
    if (qAbs(volts - m_p.volts) <= tol) {
        if (m_inTolCount < kInTolNeeded) ++m_inTolCount;
    } else {
        m_inTolCount = 0;
    }

    refreshArming();
    if (m_state == State::Settling) updateStateLabel();
}

void MainWindow::onMvStale()
{
    m_mvStale    = true;
    m_inTolCount = 0;
    refreshArming();
    updateStateLabel();
}

void MainWindow::onFireTimeout()
{
    ui->listLog->addItem(tr("(no DONE - train result unknown)"));
    m_inTolCount = 0;
    setState(State::Settling);
    showOutcome(tr("no reply from device"), kColFault);
}

// ------------------------------------------------------------- rx parsing

void MainWindow::onLineReceived(const QString &line)
{
    const QStringList t = line.trimmed().split(' ', Qt::SkipEmptyParts);
    if (t.isEmpty()) return;

    if (t[0] == "MV" && t.size() >= 2) {
        onMeasured(t[1].toDouble());          // streamed, not logged
        return;
    }

    qDebug() << "[RX]" << line;

    // "ACK FIRE" is two tokens; the parameter ACKs are three. Check it first.
    if (t[0] == "ACK" && t.value(1) == "FIRE") {
        setState(State::Firing);
        m_fireWatchdog->start(kFireTimeoutMs);
        ui->listLog->addItem(line);
        return;
    }

    if (t[0] == "ACK" && t.size() >= 3) {
        const double v = t[2].toDouble();
        if      (t[1] == "NP") { m_p.nPulses = int(v); ui->lcdpulses->display(int(v)); }
        else if (t[1] == "PW") { m_p.widthMs = int(v); ui->lcdduration->display(int(v)); }
        else if (t[1] == "PG") { m_p.gapMs   = int(v); ui->lcdspacing->display(int(v)); }
        else if (t[1] == "PV") {
            m_p.volts = v;
            ui->lcdsetvolt->display(QString::number(v, 'f', 1));
            // New target means not settled, whatever the last reading said.
            m_inTolCount = 0;
            refreshArming();
            updateStateLabel();
        }
        return;
    }

    if (t[0] == "PULSE" && t.size() >= 3) {
        m_fireWatchdog->start(kFireTimeoutMs);   // each pulse re-arms it
        if (!m_outcomeHeld)
            ui->lblState->setText(tr("pulse %1 of %2").arg(t[1], t[2]));
        return;
    }

    if (t[0] == "DONE") {
        m_fireWatchdog->stop();
        ui->listLog->addItem(line);

        const bool aborted = (t.value(1) == "ABORT");
        m_inTolCount = 0;
        setState(State::Settling);
        if (aborted) ui->pollo->setPixmap(QPixmap(":/asustado.png"));
        showOutcome(aborted ? tr("aborted")
                            : tr("done - %1 pulses").arg(t.value(1)),
                    aborted ? kColSettling : kColArmed);
        return;
    }

    if (t[0] == "FAULT") {
        setState(State::Fault);
        ui->listLog->addItem(line);
        return;
    }

    if (t[0] == "NAK") {
        ui->listLog->addItem(line);
        if (t.value(1) == "FIRE") {
            m_fireWatchdog->stop();
            m_inTolCount = 0;
            setState(State::Settling);
            showOutcome(tr("fire refused: %1").arg(t.value(2, tr("?"))), kColFault);
        } else {
            ui->lblStatus->setText(tr("Device rejected %1").arg(t.value(1)));
        }
        return;
    }

    ui->listLog->addItem(line);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Back && m_stack->currentWidget() == m_scanPage) {
        showMainPage();
        e->accept();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

// ---------------------------------------------------------------- actions

void MainWindow::onActivateClicked()
{
    if (!m_link->isReady()) { ui->lblStatus->setText(tr("Not connected")); return; }

    // The button is only enabled in Armed, and the firmware re-checks anyway.
    m_link->send(QStringLiteral("FIRE"));
    if (!m_outcomeHeld) ui->lblState->setText(tr("fire requested..."));
}

void MainWindow::onAbortClicked()
{
    if (!m_link->isReady()) return;
    m_link->send(QStringLiteral("ABORT"));
}

void MainWindow::askAndSend(const QString &title, const QString &unit,
                            const QString &key, double cur,
                            double lo, double hi, int decimals)
{
    if (!m_link->isReady()) { ui->lblStatus->setText(tr("Not connected")); return; }

    QInputDialog dlg(this);
    dlg.setInputMode(QInputDialog::DoubleInput);
    dlg.setWindowTitle(title);
    dlg.setLabelText(tr("%1 to %2 %3").arg(lo).arg(hi).arg(unit));
    dlg.setDoubleRange(lo, hi);
    dlg.setDoubleDecimals(decimals);
    dlg.setDoubleValue(cur);
    dlg.setDoubleStep(decimals == 0 ? 1.0 : 0.5);

    // Instrument Light again. The dialog is a top-level window, so MainWindow's
    // stylesheet does not reach it - these rules have to be repeated here.
    dlg.setStyleSheet(R"(
        QInputDialog, QDialog {
            background-color: #eef2f5;
        }
        QLabel {
            color: #5c6f7e;
            font-size: 24px;
            padding: 8px;
        }
        QDoubleSpinBox {
            color: #59d3e8;
            background-color: #0e2029;
            selection-background-color: #1264a3;
            selection-color: #ffffff;
            font-size: 40px;
            min-height: 80px;
            padding: 4px 8px;
            border: 1px solid #21323d;
            border-radius: 6px;
        }
        QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
            width: 70px;
            background-color: #1b3240;
        }
        QPushButton {
            color: #16232e;
            background-color: #ffffff;
            font-size: 22px;
            min-height: 64px;
            min-width: 130px;
            border: 1px solid #c8d2da;
            border-radius: 8px;
        }
        QPushButton:pressed {
            background-color: #dbe6ef;
            border-color: #1264a3;
        }
    )");

    if (dlg.exec() != QDialog::Accepted) return;
    const double v = dlg.doubleValue();

    m_link->send(QStringLiteral("SET %1 %2").arg(key).arg(v, 0, 'f', decimals));
    // LCD deliberately not touched here - it updates on ACK.
}

void MainWindow::on_npulses_clicked()
{ askAndSend(tr("Pulses"), "", "NP", m_p.nPulses, 2, 7, 0); }

void MainWindow::on_pulseduration_clicked()
{ askAndSend(tr("Duration"), "ms", "PW", m_p.widthMs, 10, 70, 0); }

void MainWindow::on_pulsespacing_clicked()
{ askAndSend(tr("Spacing"), "ms", "PG", m_p.gapMs, 25, 200, 0); }

void MainWindow::on_setvolt_clicked()
{ askAndSend(tr("Voltage"), "V", "PV", m_p.volts, 2, 45, 1); }
