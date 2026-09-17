#include "ui/devicedialog.h"

#include "comm/protocolregistry.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

DeviceDialog::DeviceDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    loadPoints(defaultTagPoints());
    onProtocolChanged();
}

void DeviceDialog::setupUi()
{
    setWindowTitle(QStringLiteral("设备配置"));
    resize(660, 600);

    auto *form = new QFormLayout;

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(QStringLiteral("例如：1 号空压机"));

    // 可编辑分组下拉：既能选已有分组，也能直接敲一个新组名。
    m_groupBox = new QComboBox(this);
    m_groupBox->setEditable(true);
    m_groupBox->setInsertPolicy(QComboBox::NoInsert);
    m_groupBox->addItem(defaultGroupName());
    if (auto *edit = m_groupBox->lineEdit())
        edit->setPlaceholderText(QStringLiteral("可直接输入新分组名"));

    m_protocolBox = new QComboBox(this);
    // 协议下拉的内容直接来自协议注册表：**本文件里没有任何一个具体协议的名字**。
    // 好处是外部插件（编译期之后才放进 plugins/protocols/ 的动态库）装进来之后，
    // 这里会自动多出一个选项，界面代码一行都不用改。
    for (IProtocolPlugin *plugin : ProtocolRegistry::instance().plugins())
        m_protocolBox->addItem(plugin->displayName(), plugin->id());

    m_hostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), this);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(502);

    m_slaveSpin = new QSpinBox(this);
    m_slaveSpin->setRange(1, 247);
    m_slaveSpin->setValue(1);

    m_intervalSpin = new QSpinBox(this);
    m_intervalSpin->setRange(100, 60000);
    m_intervalSpin->setSingleStep(100);
    m_intervalSpin->setValue(1000);
    m_intervalSpin->setSuffix(QStringLiteral(" ms"));

    m_topicEdit = new QLineEdit(this);
    m_topicEdit->setPlaceholderText(QStringLiteral("例如：factory/line1/#"));

    // MQTT 接入账号：留空即匿名接入（内网 broker 的常见配置）
    m_userEdit = new QLineEdit(this);
    m_userEdit->setPlaceholderText(QStringLiteral("留空 = 匿名接入"));

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QStringLiteral("留空 = 不发送密码"));

    m_slaveLabel = new QLabel(QStringLiteral("从站地址"), this);
    m_topicLabel = new QLabel(QStringLiteral("订阅主题"), this);
    m_userLabel = new QLabel(QStringLiteral("接入账号"), this);
    m_passwordLabel = new QLabel(QStringLiteral("接入密码"), this);
    // 地址 / 端口的标签也要存下来：不走网络的协议（如模拟设备）要把它们一起藏掉，
    // 只留一个孤零零的输入框在那儿很费解。
    m_hostLabel = new QLabel(QStringLiteral("地址 / 主机"), this);
    m_portLabel = new QLabel(QStringLiteral("端口"), this);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setObjectName(QStringLiteral("panelHint"));
    m_hintLabel->setWordWrap(true);

    form->addRow(QStringLiteral("设备名称"), m_nameEdit);
    form->addRow(QStringLiteral("所属分组"), m_groupBox);
    form->addRow(QStringLiteral("通信协议"), m_protocolBox);
    form->addRow(m_hostLabel, m_hostEdit);
    form->addRow(m_portLabel, m_portSpin);
    form->addRow(m_slaveLabel, m_slaveSpin);
    form->addRow(m_topicLabel, m_topicEdit);
    form->addRow(m_userLabel, m_userEdit);
    form->addRow(m_passwordLabel, m_passwordEdit);
    form->addRow(QStringLiteral("采集周期"), m_intervalSpin);

    m_pointTable = new QTableWidget(0, 6, this);
    m_pointTable->setHorizontalHeaderLabels({QStringLiteral("点位 ID"),
                                             QStringLiteral("显示名"),
                                             QStringLiteral("单位"),
                                             QStringLiteral("寄存器地址"),
                                             QStringLiteral("寄存器类型"),
                                             QStringLiteral("缩放")});
    m_pointTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pointTable->verticalHeader()->setVisible(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *hint = m_hintLabel;

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(m_pointTable);
    layout->addWidget(buttons);

    connect(m_protocolBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeviceDialog::onProtocolChanged);
}

QString DeviceDialog::currentProtocolId() const
{
    return m_protocolBox->currentData().toString();
}

void DeviceDialog::onProtocolChanged()
{
    const QString protocolId = currentProtocolId();

    // 端口跟随协议给默认值（切协议时重置，避免残留上一个协议的端口）。
    // 不走网络的协议默认端口是 0 —— 那种情况下保持原值，别把 502 硬塞给它。
    const quint16 defaultPort = defaultPortForProtocol(protocolId);
    if (defaultPort != 0)
        m_portSpin->setValue(defaultPort);

    applyProtocolVisibility();
    updateHint(protocolId);
}

void DeviceDialog::applyProtocolVisibility()
{
    // 显示哪些字段由**插件自己声明**（ProtocolTraits），这里不再判断协议名。
    // 于是新增协议时，"界面要动的地方"是零。
    const ProtocolTraits traits = ProtocolRegistry::instance().traits(currentProtocolId());

    const auto setRowVisible = [](QLabel *label, QWidget *field, bool visible) {
        label->setVisible(visible);
        field->setVisible(visible);
    };

    setRowVisible(m_hostLabel, m_hostEdit, traits.usesNetwork);
    setRowVisible(m_portLabel, m_portSpin, traits.usesNetwork);
    setRowVisible(m_slaveLabel, m_slaveSpin, traits.usesSlaveId);
    setRowVisible(m_topicLabel, m_topicEdit, traits.usesEndpoint);
    setRowVisible(m_userLabel, m_userEdit, traits.usesCredentials);
    setRowVisible(m_passwordLabel, m_passwordEdit, traits.usesCredentials);

    // 端点在界面上的叫法也由插件决定：MQTT 叫「订阅主题」，HTTP 叫「请求路径」——
    // 同一个配置字段，用各自协议的话说。
    if (traits.usesEndpoint) {
        m_topicLabel->setText(traits.endpointLabel.isEmpty() ? QStringLiteral("订阅主题")
                                                             : traits.endpointLabel);
    }
}

void DeviceDialog::updateHint(const QString &protocolId)
{
    IProtocolPlugin *plugin = ProtocolRegistry::instance().plugin(protocolId);
    const QString description = plugin ? plugin->description() : QString();

    // 点位表的填写方式跟协议有关，这部分是按"需要什么类型的信息"描述的，
    // 不是按协议名硬编码的说明文案。
    const ProtocolTraits traits = ProtocolRegistry::instance().traits(protocolId);
    QStringList tips;
    if (traits.usesSlaveId)
        tips << QStringLiteral("点位表用「寄存器地址 + 寄存器类型」定位，开关量只取 0/1、不做缩放。");
    if (traits.usesEndpoint)
        tips << QStringLiteral("「%1」由协议自己解释；点位表的 ID 要和现场返回的字段名对上。")
                    .arg(traits.endpointLabel.isEmpty() ? QStringLiteral("订阅主题")
                                                        : traits.endpointLabel);
    if (traits.usesCredentials)
        tips << QStringLiteral("接入账号 / 密码留空即匿名接入（密码按明文保存，仅适合内网）。");

    QStringList lines;
    if (!description.isEmpty())
        lines << description;
    lines += tips;
    m_hintLabel->setText(lines.join(QStringLiteral("\n")));
    m_hintLabel->setVisible(!lines.isEmpty());
}

void DeviceDialog::loadPoints(const QList<TagPoint> &points)
{
    m_pointTable->setRowCount(0);

    for (const TagPoint &point : points) {
        const int row = m_pointTable->rowCount();
        m_pointTable->insertRow(row);

        m_pointTable->setItem(row, 0, new QTableWidgetItem(point.id));
        m_pointTable->setItem(row, 1, new QTableWidgetItem(point.name));
        m_pointTable->setItem(row, 2, new QTableWidgetItem(point.unit));
        m_pointTable->setItem(row, 3, new QTableWidgetItem(QString::number(point.address)));

        auto *typeBox = new QComboBox(m_pointTable);
        typeBox->addItem(QStringLiteral("线圈 (01)"), 1);
        typeBox->addItem(QStringLiteral("保持寄存器 (03)"), 3);
        typeBox->addItem(QStringLiteral("输入寄存器 (04)"), 4);
        const int typeIndex = typeBox->findData(point.registerType);
        typeBox->setCurrentIndex(typeIndex >= 0 ? typeIndex : 1);
        m_pointTable->setCellWidget(row, 4, typeBox);

        m_pointTable->setItem(row, 5, new QTableWidgetItem(QString::number(point.scale)));
    }
}

QList<TagPoint> DeviceDialog::collectPoints() const
{
    QList<TagPoint> points;

    for (int row = 0; row < m_pointTable->rowCount(); ++row) {
        const auto cellText = [this, row](int column) {
            const QTableWidgetItem *item = m_pointTable->item(row, column);
            return item ? item->text().trimmed() : QString();
        };

        TagPoint point;
        point.id = cellText(0);
        if (point.id.isEmpty())
            continue; // 没填点位 ID 的行视为空行，跳过

        point.name = cellText(1);
        point.unit = cellText(2);
        point.address = cellText(3).toInt();

        if (const auto *box = qobject_cast<QComboBox *>(m_pointTable->cellWidget(row, 4)))
            point.registerType = box->currentData().toInt();

        point.scale = cellText(5).toDouble();
        if (point.scale == 0.0)
            point.scale = 1.0;
        point.boolean = (point.registerType == 1);

        points.append(point);
    }

    return points;
}

void DeviceDialog::setGroups(const QStringList &groups)
{
    const QString current = m_groupBox->currentText();

    m_groupBox->clear();
    for (const QString &group : groups) {
        if (!group.trimmed().isEmpty())
            m_groupBox->addItem(group);
    }
    if (m_groupBox->count() == 0)
        m_groupBox->addItem(defaultGroupName());

    // 优先恢复原来选中的分组，否则落在第一项。
    if (!current.trimmed().isEmpty())
        m_groupBox->setCurrentText(current);
    else
        m_groupBox->setCurrentIndex(0);
}

void DeviceDialog::setDevice(const DeviceInfo &device)
{
    m_device = device;

    m_nameEdit->setText(device.name);
    m_groupBox->setCurrentText(device.groupName());

    // 按协议 **id** 找选项（不是下标 —— 下标会随插件增删而变，id 不会）
    const int protocolIndex = m_protocolBox->findData(device.protocolId);
    const bool protocolMissing = (protocolIndex < 0);
    if (!protocolMissing)
        m_protocolBox->setCurrentIndex(protocolIndex);

    m_hostEdit->setText(device.host);
    m_slaveSpin->setValue(device.slaveId);
    m_intervalSpin->setValue(device.pollIntervalMs);
    m_topicEdit->setText(device.mqttTopic);
    m_userEdit->setText(device.username);
    m_passwordEdit->setText(device.password);

    // 端口放在协议之后设置，避免被 onProtocolChanged 的默认值覆盖。
    m_portSpin->setValue(device.port);

    loadPoints(device.points.isEmpty() ? defaultTagPoints() : device.points);
    applyProtocolVisibility();

    if (protocolMissing) {
        // 协议插件不在了（被删掉 / 配置来自另一台机器）：**明确提示**，
        // 绝不静默把设备改成下拉里的第一项 —— 那等于偷偷换了一种通信协议。
        m_hintLabel->setText(QStringLiteral("⚠ 找不到协议插件「%1」：该协议未安装。"
                                            "保存前请重新选择一种协议，否则采集无法启动。")
                                 .arg(device.protocolId));
        m_hintLabel->setVisible(true);
    }
}

DeviceInfo DeviceDialog::device() const
{
    DeviceInfo info = m_device; // 保留原 id（编辑模式）

    info.name = m_nameEdit->text().trimmed();
    info.group = m_groupBox->currentText().trimmed();
    if (info.group.isEmpty())
        info.group = defaultGroupName();
    info.protocolId = currentProtocolId();
    info.host = m_hostEdit->text().trimmed();
    info.port = static_cast<quint16>(m_portSpin->value());
    info.slaveId = m_slaveSpin->value();
    info.pollIntervalMs = m_intervalSpin->value();
    info.mqttTopic = m_topicEdit->text().trimmed();
    info.points = collectPoints();

    // 账号只在声明需要它的协议下保留：切回别的协议时把残留的账号清掉，
    // 否则导出配置里会带着一份用不上的明文口令。
    if (ProtocolRegistry::instance().traits(info.protocolId).usesCredentials) {
        info.username = m_userEdit->text().trimmed();
        // 密码不做 trim：口令里的首尾空格是有效字符，替用户"修正"反而会造成登录失败
        info.password = m_passwordEdit->text();
    } else {
        info.username.clear();
        info.password.clear();
    }

    if (info.name.isEmpty())
        info.name = protocolName(info.protocolId);

    return info;
}
