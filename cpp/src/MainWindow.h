#pragma once
#include "Settings.h"

#include <QFile>
#include <QMainWindow>

class Gateway;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QSystemTrayIcon;

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr);
    void startGateway();
    void stopGateway();

  protected:
    void closeEvent(QCloseEvent *e) override;

  private:
    QWidget *buildGatewayTab();
    QWidget *buildMeshTab();
    QWidget *buildJs8Tab();
    QWidget *buildForwardTab();
    QWidget *buildOptionsTab();
    void buildTray();
    void showSettings(const Settings &s);
    Settings collectSettings() const;
    void save();
    void appendLog(const QString &line);
    void updateButtons();
    void scanBluetooth();
    void refreshSerialPorts();
    void quitApp();

    Gateway *m_gw;
    Settings m_settings;
    QFile m_logFile;
    bool m_quitting = false;

    // gateway tab
    QLabel *m_meshState, *m_js8State;
    QPushButton *m_startBtn, *m_stopBtn;
    QPlainTextEdit *m_log;
    // mesh tab
    QComboBox *m_meshType;
    QStackedWidget *m_meshPages;
    QLineEdit *m_bleDevice, *m_meshTcpHost, *m_allowed;
    QComboBox *m_serialPort;
    QSpinBox *m_meshTcpPort, *m_hopLimit;
    // js8 tab
    QComboBox *m_js8Type;
    QStackedWidget *m_js8Pages;
    QLineEdit *m_url, *m_passcode, *m_js8Host;
    QSpinBox *m_relogin, *m_js8Port;
    // forwarding & limits
    QRadioButton *m_groupsNone, *m_groupsAll, *m_groupsList;
    QLineEdit *m_groups;
    QSpinBox *m_inboxCount, *m_meshChunk, *m_dedupe, *m_heardMin, *m_heardCount, *m_txReport;
    QDoubleSpinBox *m_meshGap;
    // options
    QCheckBox *m_startOnLaunch, *m_closeToTray;
    QSystemTrayIcon *m_tray = nullptr;
};
