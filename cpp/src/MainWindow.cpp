#include "MainWindow.h"
#include "QtCompat.h"
#include "BleMeshLink.h"
#include "Gateway.h"

#include <QApplication>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothUuid>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
const QBluetoothUuid kMeshtasticService(QStringLiteral("6ba1b218-15a8-461f-9fa8-5dcae273eafd"));
constexpr qint64 kLogMaxBytes = 5 * 1024 * 1024;

// The app icon, drawn here so there are no image files to ship.
QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 32, 64, 128}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(0x1f, 0x6f, 0xb2));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(0.5, 0.5, size - 1, size - 1));
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(int(size * 0.42));
        p.setFont(f);
        p.drawText(pm.rect(), Qt::AlignCenter, "J8");
        icon.addPixmap(pm);
    }
    return icon;
}

QLabel *note(const QString &text) {
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet("color: palette(mid);");
    return l;
}

QSpinBox *spin(int lo, int hi, const QString &suffix = {}) {
    auto *s = new QSpinBox;
    s->setRange(lo, hi);
    s->setSuffix(suffix);
    return s;
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_gw(new Gateway(this)) {
    setWindowTitle(tr("JS8 Mesh Gateway"));
    setWindowIcon(appIcon());

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    m_logFile.setFileName(dir + "/js8mesh.log");
    if (m_logFile.size() > kLogMaxBytes) {
        QFile::remove(m_logFile.fileName() + ".1");
        QFile::rename(m_logFile.fileName(), m_logFile.fileName() + ".1");
    }
    m_logFile.open(QIODevice::Append | QIODevice::Text);

    auto *tabs = new QTabWidget;
    tabs->addTab(buildGatewayTab(), tr("Gateway"));
    tabs->addTab(buildMeshTab(), tr("Meshtastic node"));
    tabs->addTab(buildJs8Tab(), tr("Subspace station"));
    tabs->addTab(buildForwardTab(), tr("Forwarding && limits"));
    tabs->addTab(buildOptionsTab(), tr("Options"));

    auto *saveBtn = new QPushButton(tr("Save settings"));
    auto *revertBtn = new QPushButton(tr("Undo changes"));
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::save);
    connect(revertBtn, &QPushButton::clicked, this, [this]() { showSettings(m_settings); });
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(note(tr("Settings take effect when saved; a running gateway restarts.")), 1);
    bottom->addWidget(revertBtn);
    bottom->addWidget(saveBtn);

    auto *central = new QWidget;
    auto *v = new QVBoxLayout(central);
    v->addWidget(tabs);
    v->addLayout(bottom);
    setCentralWidget(central);
    resize(760, 560);

    connect(m_gw, &Gateway::logMessage, this, &MainWindow::appendLog);
    connect(m_gw, &Gateway::meshStatus, this, [this](const QString &t, bool ok) {
        m_meshState->setText((ok ? "● " : "○ ") + t);
        m_meshState->setStyleSheet(ok ? "color: #2a8a2a;" : "");
    });
    connect(m_gw, &Gateway::js8Status, this, [this](const QString &t, bool ok) {
        m_js8State->setText((ok ? "● " : "○ ") + t);
        m_js8State->setStyleSheet(ok ? "color: #2a8a2a;" : "");
    });
    connect(m_gw, &Gateway::settingsChanged, this, [this](const Settings &s) {
        // a mesh command (GROUPS) changed a setting; show it, keeping other unsaved edits
        m_settings.groupsMode = s.groupsMode;
        m_settings.groups = s.groups;
        m_groupsNone->setChecked(s.groupsMode == Settings::GroupsNone);
        m_groupsAll->setChecked(s.groupsMode == Settings::GroupsAll);
        m_groupsList->setChecked(s.groupsMode == Settings::GroupsList);
        m_groups->setText(s.groups.join(' '));
    });

    m_settings.load();
    showSettings(m_settings);
    buildTray();
    updateButtons();
}

// ---- tabs -----------------------------------------------------------------------------
QWidget *MainWindow::buildGatewayTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *status = new QGroupBox(tr("Status"));
    auto *f = new QFormLayout(status);
    m_meshState = new QLabel(tr("○ stopped"));
    m_js8State = new QLabel(tr("○ stopped"));
    // long status text wraps; it never widens the window
    for (QLabel *l : {m_meshState, m_js8State}) {
        l->setWordWrap(true);
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    f->addRow(tr("Meshtastic node:"), m_meshState);
    f->addRow(tr("JS8 station:"), m_js8State);
    m_startBtn = new QPushButton(tr("Start gateway"));
    m_stopBtn = new QPushButton(tr("Stop gateway"));
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::startGateway);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::stopGateway);
    auto *btns = new QHBoxLayout;
    btns->addWidget(m_startBtn);
    btns->addWidget(m_stopBtn);
    btns->addStretch();
    f->addRow(btns);
    v->addWidget(status);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto *logLabel = new QLabel(tr("Log (also saved to %1):").arg(QDir::toNativeSeparators(m_logFile.fileName())));
    logLabel->setWordWrap(true);
    logLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    logLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(logLabel);
    v->addWidget(m_log, 1);
    return w;
}

QWidget *MainWindow::buildMeshTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *f = new QFormLayout;
    m_meshType = new QComboBox;
    m_meshType->addItems({tr("Bluetooth LE"), tr("USB serial"), tr("TCP (node on WiFi/Ethernet)")});
    f->addRow(tr("Connect by:"), m_meshType);
    v->addLayout(f);

    m_meshPages = new QStackedWidget;
    // Bluetooth
    auto *ble = new QWidget;
    auto *bf = new QFormLayout(ble);
    m_bleDevice = new QLineEdit;
    m_bleDevice->setPlaceholderText(tr("e.g. Rpt1_be91"));
    // a name typed by hand: forget the address the scan found for the old one
    connect(m_bleDevice, &QLineEdit::textEdited, this, [this]() { m_bleAddress.clear(); });
    auto *scan = new QPushButton(tr("Scan..."));
    connect(scan, &QPushButton::clicked, this, &MainWindow::scanBluetooth);
    auto *bh = new QHBoxLayout;
    bh->addWidget(m_bleDevice, 1);
    bh->addWidget(scan);
    bf->addRow(tr("Node's Bluetooth name or address:"), bh);
    bf->addRow(note(tr("Pair the node once in your system's Bluetooth settings (its PIN is on its screen, "
                       "or 123456). While the gateway is connected, the phone app can't connect to this node.")));
    m_meshPages->addWidget(ble);
    // Serial
    auto *ser = new QWidget;
    auto *sf = new QFormLayout(ser);
    m_serialPort = new QComboBox;
    m_serialPort->setEditable(true);
    auto *refresh = new QPushButton(tr("Refresh"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshSerialPorts);
    auto *sh = new QHBoxLayout;
    sh->addWidget(m_serialPort, 1);
    sh->addWidget(refresh);
    sf->addRow(tr("Serial port:"), sh);
    m_meshPages->addWidget(ser);
    // TCP
    auto *tcp = new QWidget;
    auto *tf = new QFormLayout(tcp);
    m_meshTcpHost = new QLineEdit;
    m_meshTcpHost->setPlaceholderText(tr("e.g. 192.168.1.60 or meshtastic.local"));
    m_meshTcpPort = spin(1, 65535);
    tf->addRow(tr("Node's address:"), m_meshTcpHost);
    tf->addRow(tr("Port:"), m_meshTcpPort);
    m_meshPages->addWidget(tcp);
    connect(m_meshType, QOverload<int>::of(&QComboBox::currentIndexChanged), m_meshPages, &QStackedWidget::setCurrentIndex);
    v->addWidget(m_meshPages);

    auto *f2 = new QFormLayout;
    m_allowed = new QLineEdit;
    m_allowed->setPlaceholderText(tr("e.g. !8a31cb4a"));
    f2->addRow(tr("Allowed nodes:"), m_allowed);
    f2->addRow(note(tr("Node IDs (from the Meshtastic app's node list), separated by spaces or commas. "
                       "Only these nodes can send commands, and received JS8 messages go to them.")));
    m_hopLimit = spin(1, 7);
    f2->addRow(tr("Hop limit for replies:"), m_hopLimit);
    v->addLayout(f2);
    v->addStretch();
    refreshSerialPorts();
    return w;
}

QWidget *MainWindow::buildJs8Tab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *rf = new QFormLayout;
    m_url = new QLineEdit;
    m_url->setPlaceholderText("http://192.168.1.50:2443");
    m_passcode = new QLineEdit;
    m_passcode->setEchoMode(QLineEdit::Password);
    auto *show = new QCheckBox(tr("Show"));
    connect(show, &QCheckBox::toggled, this,
            [this](bool on) { m_passcode->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    auto *ph = new QHBoxLayout;
    ph->addWidget(m_passcode, 1);
    ph->addWidget(show);
    m_relogin = spin(10, 3600, tr(" s"));
    rf->addRow(tr("Address (Home network):"), m_url);
    rf->addRow(tr("Pass code:"), ph);
    rf->addRow(tr("After a phone logs in, wait:"), m_relogin);
    rf->addRow(note(tr("In Subspace: File > Phone remote > \"Allow a phone to operate this station\". Use the "
                       "Home network address and pass code it shows. The remote allows one login at a time: a "
                       "phone logging in logs the gateway out, and after the wait above the gateway logs back in "
                       "(logging the phone out). The pass code is stored in this app's settings.")));
    v->addLayout(rf);
    v->addStretch();
    return w;
}

QWidget *MainWindow::buildForwardTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *g = new QGroupBox(tr("Forward JS8 messages to the mesh"));
    auto *gv = new QVBoxLayout(g);
    gv->addWidget(new QLabel(tr("Messages to your callsign are always forwarded. Also forward @groups:")));
    m_groupsNone = new QRadioButton(tr("None"));
    m_groupsAll = new QRadioButton(tr("All groups (except @HB heartbeats)"));
    m_groupsList = new QRadioButton(tr("Only these:"));
    m_groups = new QLineEdit;
    m_groups->setPlaceholderText(tr("e.g. @MAGNET @ALLCALL"));
    auto *gh = new QHBoxLayout;
    gh->addWidget(m_groupsList);
    gh->addWidget(m_groups, 1);
    gv->addWidget(m_groupsNone);
    gv->addWidget(m_groupsAll);
    gv->addLayout(gh);
    gv->addWidget(note(tr("The GROUPS command (GR) changes this from the mesh.")));
    v->addWidget(g);

    auto *l = new QGroupBox(tr("Limits"));
    auto *f = new QFormLayout(l);
    m_inboxCount = spin(1, 50);
    m_meshChunk = spin(40, 228, tr(" characters"));
    m_meshGap = new QDoubleSpinBox;
    m_meshGap->setRange(0.5, 60);
    m_meshGap->setSuffix(tr(" s"));
    m_dedupe = spin(0, 86400, tr(" s"));
    m_heardMin = spin(1, 1440, tr(" min"));
    m_heardCount = spin(1, 50);
    m_txReport = spin(30, 3600, tr(" s"));
    f->addRow(tr("INBOX lists:"), m_inboxCount);
    f->addRow(tr("Longest mesh message:"), m_meshChunk);
    f->addRow(tr("Gap between mesh messages:"), m_meshGap);
    f->addRow(tr("Ignore a repeated JS8 message within:"), m_dedupe);
    f->addRow(tr("HEARD covers the last:"), m_heardMin);
    f->addRow(tr("HEARD lists up to:"), m_heardCount);
    f->addRow(tr("Report a transmission as missing after:"), m_txReport);
    v->addWidget(l);
    v->addStretch();
    return w;
}

QWidget *MainWindow::buildOptionsTab() {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_startOnLaunch = new QCheckBox(tr("Start the gateway when this app opens"));
    m_closeToTray = new QCheckBox(tr("Closing the window keeps the gateway running in the system tray"));
    v->addWidget(m_startOnLaunch);
    v->addWidget(m_closeToTray);
    v->addWidget(note(tr("To start with your PC, add this app to your startup programs.")));
    v->addStretch();
    v->addWidget(note(tr("js8mesh %1 - https://github.com/avantol/Subspace-Meshtastic-Basic").arg(JS8MESH_VERSION)));
    return w;
}

void MainWindow::buildTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    m_tray = new QSystemTrayIcon(appIcon(), this);
    m_tray->setToolTip(windowTitle());
    auto *menu = new QMenu(this);
    menu->addAction(tr("Show"), this, [this]() { showNormal(); raise(); activateWindow(); });
    menu->addAction(tr("Start gateway"), this, &MainWindow::startGateway);
    menu->addAction(tr("Stop gateway"), this, &MainWindow::stopGateway);
    menu->addSeparator();
    menu->addAction(tr("Quit"), this, &MainWindow::quitApp);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger) {
            if (isVisible()) hide();
            else { showNormal(); raise(); activateWindow(); }
        }
    });
    m_tray->show();
}

// ---- settings <-> widgets -------------------------------------------------------------------
void MainWindow::showSettings(const Settings &s) {
    m_meshType->setCurrentIndex(int(s.meshType));
    m_meshPages->setCurrentIndex(int(s.meshType));
    m_bleDevice->setText(s.bleDevice);
    m_bleAddress = s.bleAddress;
    m_serialPort->setCurrentText(s.serialPort);
    m_meshTcpHost->setText(s.meshTcpHost);
    m_meshTcpPort->setValue(s.meshTcpPort);
    m_allowed->setText(s.allowedNodes.join(' '));
    m_hopLimit->setValue(s.hopLimit);
    m_url->setText(s.subspaceUrl);
    m_passcode->setText(s.subspacePasscode);
    m_relogin->setValue(s.reloginSeconds);
    m_groupsNone->setChecked(s.groupsMode == Settings::GroupsNone);
    m_groupsAll->setChecked(s.groupsMode == Settings::GroupsAll);
    m_groupsList->setChecked(s.groupsMode == Settings::GroupsList);
    m_groups->setText(s.groups.join(' '));
    m_inboxCount->setValue(s.inboxCount);
    m_meshChunk->setValue(s.meshChunk);
    m_meshGap->setValue(s.meshGapSeconds);
    m_dedupe->setValue(s.dedupeSeconds);
    m_heardMin->setValue(s.heardSeconds / 60);
    m_heardCount->setValue(s.heardCount);
    m_txReport->setValue(s.txReportSeconds);
    m_startOnLaunch->setChecked(s.startOnLaunch);
    m_closeToTray->setChecked(s.closeToTray);
}

Settings MainWindow::collectSettings() const {
    static const QRegularExpression sep(QStringLiteral("[\\s,;]+"));
    Settings s;
    s.meshType = Settings::MeshType(m_meshType->currentIndex());
    s.bleDevice = BleMeshLink::cleanName(m_bleDevice->text());
    s.bleAddress = m_bleAddress;
    s.serialPort = m_serialPort->currentText().trimmed();
    s.meshTcpHost = m_meshTcpHost->text().trimmed();
    s.meshTcpPort = m_meshTcpPort->value();
    for (QString n : m_allowed->text().split(sep, Qt::SkipEmptyParts)) {
        n = n.toLower();
        if (!n.startsWith('!')) n.prepend('!');
        s.allowedNodes << n;
    }
    s.hopLimit = m_hopLimit->value();
    s.subspaceUrl = m_url->text().trimmed();
    s.subspacePasscode = m_passcode->text().trimmed();
    s.reloginSeconds = m_relogin->value();
    s.groupsMode = m_groupsAll->isChecked() ? Settings::GroupsAll
                 : m_groupsList->isChecked() ? Settings::GroupsList
                                             : Settings::GroupsNone;
    for (QString g : m_groups->text().toUpper().split(sep, Qt::SkipEmptyParts)) {
        if (!g.startsWith('@')) g.prepend('@');
        if (!s.groups.contains(g)) s.groups << g;
    }
    s.inboxCount = m_inboxCount->value();
    s.meshChunk = m_meshChunk->value();
    s.meshGapSeconds = m_meshGap->value();
    s.dedupeSeconds = m_dedupe->value();
    s.heardSeconds = m_heardMin->value() * 60;
    s.heardCount = m_heardCount->value();
    s.txReportSeconds = m_txReport->value();
    s.startOnLaunch = m_startOnLaunch->isChecked();
    s.closeToTray = m_closeToTray->isChecked();
    return s;
}

void MainWindow::save() {
    Settings s = collectSettings();
    QString problem = s.validate();
    if (!problem.isEmpty() && m_gw->isRunning()) {
        QMessageBox::warning(this, windowTitle(), problem);
        return;
    }
    m_settings = s;
    m_settings.save();
    showSettings(m_settings);
    appendLog(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss ") + tr("Settings saved."));
    if (!problem.isEmpty()) QMessageBox::information(this, windowTitle(), tr("Saved. Before starting: %1").arg(problem));
    if (m_gw->isRunning()) startGateway(); // restart with the new settings
}

// ---- gateway control ------------------------------------------------------------------------
void MainWindow::startGateway() {
    // start with what the window shows: an unsaved edit (like a node picked in Scan) counts
    Settings shown = collectSettings();
    if (shown.validate().isEmpty() || !m_settings.validate().isEmpty()) {
        m_settings = shown;
        m_settings.save();
        showSettings(m_settings);
    }
    QString problem = m_settings.validate();
    if (!problem.isEmpty()) {
        showNormal();
        QMessageBox::warning(this, windowTitle(), problem);
        return;
    }
    m_gw->start(m_settings);
    updateButtons();
}

void MainWindow::stopGateway() {
    if (!m_gw->isRunning()) return;
    m_gw->stop();
    appendLog(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss ") + tr("Gateway stopped."));
    m_meshState->setText(tr("○ stopped"));
    m_meshState->setStyleSheet("");
    m_js8State->setText(tr("○ stopped"));
    m_js8State->setStyleSheet("");
    updateButtons();
}

void MainWindow::updateButtons() {
    m_startBtn->setEnabled(!m_gw->isRunning());
    m_stopBtn->setEnabled(m_gw->isRunning());
    if (m_tray) m_tray->setToolTip(windowTitle() + (m_gw->isRunning() ? tr(" (running)") : tr(" (stopped)")));
}

void MainWindow::appendLog(const QString &line) {
    m_log->appendPlainText(line);
    if (m_logFile.isOpen()) {
        m_logFile.write(line.toUtf8() + '\n');
        m_logFile.flush();
    }
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (!m_quitting && m_tray && m_settings.closeToTray && m_gw->isRunning()) {
        hide();
        m_tray->showMessage(windowTitle(), tr("The gateway keeps running here. Right-click for Quit."),
                            QSystemTrayIcon::Information, 4000);
        e->ignore();
        return;
    }
    m_gw->stop();
    e->accept();
    qApp->quit();
}

void MainWindow::quitApp() {
    m_quitting = true;
    close();
}

// ---- helpers --------------------------------------------------------------------------------
void MainWindow::refreshSerialPorts() {
    QString current = m_serialPort->currentText();
    m_serialPort->clear();
    for (const QSerialPortInfo &p : QSerialPortInfo::availablePorts()) {
        QString desc = p.description().isEmpty() ? p.manufacturer() : p.description();
        m_serialPort->addItem(p.portName(), desc);
        m_serialPort->setItemData(m_serialPort->count() - 1, desc, Qt::ToolTipRole);
    }
    m_serialPort->setCurrentText(current);
}

void MainWindow::scanBluetooth() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Find the Meshtastic node"));
    auto *v = new QVBoxLayout(&dlg);
    auto *status = new QLabel(tr("Scanning for Bluetooth LE devices..."));
    auto *list = new QListWidget;
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(status);
    v->addWidget(list);
    v->addWidget(note(tr("Meshtastic nodes are listed first. If yours is missing: is it in range, powered on, "
                         "and not connected to a phone or a running gateway?")));
    v->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);

    QBluetoothDeviceDiscoveryAgent agent;
    agent.setLowEnergyDiscoveryTimeout(15000);
    auto add = [list](const QBluetoothDeviceInfo &d) {
        if (d.name().isEmpty() || !(d.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration)) return;
        bool mesh = d.serviceUuids().contains(kMeshtasticService);
        QString name = BleMeshLink::cleanName(d.name());
        if (name.isEmpty()) return;
        QString label = QStringLiteral("%1   (%2, %3 dBm)%4").arg(name, d.address().toString()).arg(d.rssi())
                            .arg(mesh ? QObject::tr("  Meshtastic") : QString());
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole + 1).toString() == d.address().toString()) {
                list->item(i)->setText(label);
                return;
            }
        auto *item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, name);
        item->setData(Qt::UserRole + 1, d.address().toString());
        if (mesh) list->insertItem(0, item);
        else list->addItem(item);
        if (!list->currentItem()) list->setCurrentRow(0);
    };
    connect(&agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, &dlg, add);
    connect(&agent, &QBluetoothDeviceDiscoveryAgent::deviceUpdated, &dlg,
            [add](const QBluetoothDeviceInfo &d, QBluetoothDeviceInfo::Fields) { add(d); });
    connect(&agent, &QBluetoothDeviceDiscoveryAgent::finished, &dlg, [status]() { status->setText(QObject::tr("Scan finished.")); });
    connect(&agent, JS8_AGENT_ERROR, &dlg,
            [status, &agent]() { status->setText(QObject::tr("Bluetooth error: %1").arg(agent.errorString())); });
    agent.start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    if (dlg.exec() == QDialog::Accepted && list->currentItem()) {
        m_bleDevice->setText(list->currentItem()->data(Qt::UserRole).toString());
        m_bleAddress = list->currentItem()->data(Qt::UserRole + 1).toString();
    }
    agent.stop();
}
