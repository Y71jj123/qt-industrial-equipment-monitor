#include "virtualdevicewindow.h"

#include "modbustcpslave.h"

#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 表格列序。
enum Column {
    ColName = 0,
    ColAddress,
    ColType,
    ColValue,
    ColRaw,
    ColOrigin,
    ColumnCount,
};

constexpr int kDefaultPort = 1502; ///< 不用 502：避免与现场真设备或需要特权的端口冲突

QString registerTypeName(int registerType)
{
    switch (registerType) {
    case 1:
        return QStringLiteral("线圈");
    case 4:
        return QStringLiteral("输入寄存器");
    default:
        return QStringLiteral("保持寄存器");
    }
}

} // namespace

VirtualDeviceWindow::VirtualDeviceWindow(quint16 autoStartPort, QWidget *parent)
    : QWidget(parent)
{
    m_slave = new ModbusTcpSlave(this);

    connect(m_slave, &ModbusTcpSlave::logMessage, this, &VirtualDeviceWindow::appendLog);
    connect(m_slave, &ModbusTcpSlave::clientCountChanged, this, [this](int) { refreshStatus(); });

    buildUi();
    buildTable();
    refreshTable();
    refreshStatus();

    // 数据发生器与监控端的默认采集周期（1000 ms）同频推进。
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, [this]() {
        m_slave->generator().step();
        refreshTable();
    });
    m_tickTimer->start();

    if (autoStartPort != 0) {
        m_portSpin->setValue(autoStartPort);
        m_slave->start(autoStartPort);
        refreshStatus();
    } else {
        appendLog(QStringLiteral("虚拟设备已就绪。点「开始监听」后，"
                                 "在监控端新建 Modbus TCP 设备（IP 127.0.0.1，端口 %1）即可连上。")
                      .arg(kDefaultPort));
    }
}

void VirtualDeviceWindow::buildUi()
{
    setWindowTitle(QStringLiteral("虚拟设备模拟器 — Modbus TCP 从站"));
    resize(820, 620);

    auto *root = new QVBoxLayout(this);

    // ---------------- 从站配置 ----------------
    auto *configGroup = new QGroupBox(QStringLiteral("从站配置"), this);
    auto *configLayout = new QHBoxLayout(configGroup);

    configLayout->addWidget(new QLabel(QStringLiteral("监听端口："), configGroup));
    m_portSpin = new QSpinBox(configGroup);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(kDefaultPort);
    configLayout->addWidget(m_portSpin);

    m_startButton = new QPushButton(QStringLiteral("开始监听"), configGroup);
    m_startButton->setMinimumWidth(110);
    connect(m_startButton, &QPushButton::clicked, this, &VirtualDeviceWindow::onStartStopClicked);
    configLayout->addWidget(m_startButton);

    configLayout->addSpacing(12);
    m_stateLabel = new QLabel(configGroup);
    configLayout->addWidget(m_stateLabel);

    configLayout->addStretch();
    m_clientLabel = new QLabel(configGroup);
    m_requestLabel = new QLabel(configGroup);
    configLayout->addWidget(m_clientLabel);
    configLayout->addSpacing(12);
    configLayout->addWidget(m_requestLabel);

    root->addWidget(configGroup);

    // ---------------- 寄存器数据 ----------------
    auto *dataGroup = new QGroupBox(QStringLiteral("寄存器数据（与监控端默认点位表对齐）"), this);
    auto *dataLayout = new QVBoxLayout(dataGroup);
    m_table = new QTableWidget(dataGroup);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({QStringLiteral("点位"),
                                        QStringLiteral("地址"),
                                        QStringLiteral("寄存器类型"),
                                        QStringLiteral("工程值"),
                                        QStringLiteral("寄存器值"),
                                        QStringLiteral("来源")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setMinimumHeight(190);
    dataLayout->addWidget(m_table);
    root->addWidget(dataGroup);

    // ---------------- 故障注入 ----------------
    auto *faultGroup = new QGroupBox(QStringLiteral("故障注入（演示监控端的告警与断线处理）"), this);
    auto *faultLayout = new QHBoxLayout(faultGroup);

    faultLayout->addWidget(new QLabel(QStringLiteral("数据模式："), faultGroup));
    m_faultCombo = new QComboBox(faultGroup);
    m_faultCombo->addItem(QStringLiteral("正常波动"));
    m_faultCombo->addItem(QStringLiteral("冻结数据（模拟死机）"));
    m_faultCombo->addItem(QStringLiteral("强制越限（触发告警）"));
    connect(m_faultCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VirtualDeviceWindow::onFaultChanged);
    faultLayout->addWidget(m_faultCombo);

    faultLayout->addSpacing(12);
    m_resumeButton = new QPushButton(QStringLiteral("恢复自动"), faultGroup);
    m_resumeButton->setToolTip(QStringLiteral("清除被下位机写入的保持值，让点位回到自动波动"));
    connect(m_resumeButton, &QPushButton::clicked, this, [this]() {
        m_slave->generator().clearOverrides();
        appendLog(QStringLiteral("已清除手动保持值，全部点位恢复自动波动"));
        refreshTable();
    });
    faultLayout->addWidget(m_resumeButton);

    m_dropButton = new QPushButton(QStringLiteral("掐断所有连接"), faultGroup);
    m_dropButton->setToolTip(QStringLiteral("演示监控端的断线检测与自动重连"));
    connect(m_dropButton, &QPushButton::clicked, this, [this]() { m_slave->dropAllClients(); });
    faultLayout->addWidget(m_dropButton);

    faultLayout->addStretch();
    root->addWidget(faultGroup);

    // ---------------- 日志 ----------------
    auto *logGroup = new QGroupBox(QStringLiteral("通信日志"), this);
    auto *logLayout = new QVBoxLayout(logGroup);
    m_log = new QPlainTextEdit(logGroup);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500); // 长跑不涨内存
    logLayout->addWidget(m_log);
    root->addWidget(logGroup, 1);
}

void VirtualDeviceWindow::buildTable()
{
    const QVector<VirtualTag> &tags = m_slave->generator().tags();

    m_table->setRowCount(tags.size());
    for (int row = 0; row < tags.size(); ++row) {
        for (int col = 0; col < ColumnCount; ++col) {
            auto *item = new QTableWidgetItem();
            if (col >= ColAddress)
                item->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, col, item);
        }
    }
}

void VirtualDeviceWindow::refreshTable()
{
    SignalGenerator &gen = m_slave->generator();
    const QVector<VirtualTag> &tags = gen.tags();

    for (int row = 0; row < tags.size(); ++row) {
        const VirtualTag &tag = tags.at(row);

        m_table->item(row, ColName)->setText(tag.name);
        m_table->item(row, ColAddress)->setText(QString::number(tag.address));
        m_table->item(row, ColType)->setText(registerTypeName(tag.registerType));

        const double value = gen.valueAt(row);
        const QString unit = tag.unit.isEmpty() ? QString() : QStringLiteral(" ") + tag.unit;
        m_table->item(row, ColValue)->setText((tag.boolean ? QString::number(int(value))
                                                          : QString::number(value, 'f', 1))
                                              + unit);

        m_table->item(row, ColRaw)->setText(QString::number(gen.rawValueAt(row)));

        QString origin = QStringLiteral("自动");
        if (gen.isOverridden(tag.address))
            origin = QStringLiteral("手动");
        else if (gen.fault() == SignalGenerator::Fault::Freeze)
            origin = QStringLiteral("冻结");
        else if (gen.fault() == SignalGenerator::Fault::OverLimit)
            origin = QStringLiteral("越限");
        m_table->item(row, ColOrigin)->setText(origin);
    }
}

void VirtualDeviceWindow::refreshStatus()
{
    const bool listening = m_slave->isListening();

    m_stateLabel->setText(listening
                              ? QStringLiteral("● 监听中 0.0.0.0:%1").arg(m_slave->port())
                              : QStringLiteral("○ 未监听"));
    m_stateLabel->setStyleSheet(listening ? QStringLiteral("color:#1a7f37;font-weight:600;")
                                         : QStringLiteral("color:#888;"));

    m_clientLabel->setText(QStringLiteral("客户端：%1").arg(m_slave->clientCount()));
    m_requestLabel->setText(QStringLiteral("累计请求：%1").arg(m_slave->requestCount()));

    m_startButton->setText(listening ? QStringLiteral("停止监听") : QStringLiteral("开始监听"));
    m_portSpin->setEnabled(!listening);
}

void VirtualDeviceWindow::appendLog(const QString &text)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
    refreshStatus();
}

void VirtualDeviceWindow::onStartStopClicked()
{
    if (m_slave->isListening()) {
        m_slave->stop();
    } else {
        m_slave->start(static_cast<quint16>(m_portSpin->value()));
    }
    refreshStatus();
}

void VirtualDeviceWindow::onFaultChanged(int index)
{
    const auto fault = static_cast<SignalGenerator::Fault>(index);
    m_slave->generator().setFault(fault);

    if (fault != SignalGenerator::Fault::None)
        appendLog(QStringLiteral("故障注入：%1").arg(SignalGenerator::faultName(fault)));

    refreshTable();
}
