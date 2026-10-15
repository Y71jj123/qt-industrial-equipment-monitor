#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

class ModbusTcpSlave;

/// 虚拟设备主窗口：一个可独立运行的 Modbus TCP 从站模拟器。
///
/// 用途有两个：
///   1. **演示**：与监控端并排开两个窗口，数据真的经 TCP 流过去，
///      比"我代码里模拟的"有说服力；也能顺手演示断线重连、远程控制。
///   2. **验证**：真实链路（连接管理、MBAP 组帧、块读、异常响应）只有在网络上跑才算验过。
class VirtualDeviceWindow : public QWidget
{
    Q_OBJECT

public:
    /// autoStartPort 非 0 时，构造完立即在该端口开始监听（命令行 --auto-start 用）。
    explicit VirtualDeviceWindow(quint16 autoStartPort = 0, QWidget *parent = nullptr);

private:
    void buildUi();
    void buildTable();
    void refreshTable();
    void refreshStatus();
    void appendLog(const QString &text);

    void onStartStopClicked();
    void onFaultChanged(int index);

    ModbusTcpSlave *m_slave = nullptr;
    QTimer *m_tickTimer = nullptr; ///< 推进数据发生器 + 刷新界面

    QSpinBox *m_portSpin = nullptr;
    QPushButton *m_startButton = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_clientLabel = nullptr;
    QLabel *m_requestLabel = nullptr;
    QComboBox *m_faultCombo = nullptr;
    QPushButton *m_dropButton = nullptr;
    QPushButton *m_resumeButton = nullptr;
    QTableWidget *m_table = nullptr;
    QPlainTextEdit *m_log = nullptr;
};
