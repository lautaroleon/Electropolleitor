#include "scanpage.h"
#include "ui_scanpage.h"

#include <QListWidgetItem>

ScanPage::ScanPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ScanPage)
{
    ui->setupUi(this);

    // Android touch targets want ~48dp minimum.
    ui->listDevices->setStyleSheet(R"(
    QListWidget {
        background-color: #ffffff;
        color: #ffffff;
        border: none;
    }
    QListWidget::item {
        padding: 14px;
        color: #000000;
        border-bottom: 1px solid #303030;
    }
    QListWidget::item:selected {
        background-color: #2d4a63;
        color: #fff000;
    }
)");

    connect(ui->listDevices, &QListWidget::itemClicked,
            this, [this](QListWidgetItem *item) {
                emit deviceChosen(ui->listDevices->row(item));
            });

    connect(ui->btnRescan, &QPushButton::clicked,
            this, &ScanPage::rescanRequested);
    connect(ui->btnCancelScan, &QPushButton::clicked,
            this, &ScanPage::cancelled);
}

ScanPage::~ScanPage()
{
    delete ui;
}

void ScanPage::reset()
{
    ui->listDevices->clear();
    ui->listDevices->setEnabled(true);
    ui->btnRescan->setEnabled(false);
    ui->lblScanTitle->setText(tr("Scanning for electroporators..."));
}

void ScanPage::addDevice(const QString &text)
{
    ui->listDevices->addItem(text);
}

void ScanPage::setTitle(const QString &text)
{
    ui->lblScanTitle->setText(text);
}

void ScanPage::setBusy(bool busy)
{
    ui->listDevices->setEnabled(!busy);
    ui->btnRescan->setEnabled(!busy);
}

int ScanPage::deviceCount() const
{
    return ui->listDevices->count();
}
