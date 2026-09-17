#pragma once

#include "core/devicemanager.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

/// 设备配置对话框：新建 / 编辑一台设备。
///
/// 按所选协议动态显示相关字段（Modbus 显示从站地址，MQTT 显示订阅主题），
/// 并可编辑点位表。默认填充项目内置的点位模板，开箱即可用。
class DeviceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DeviceDialog(QWidget *parent = nullptr);

    /// 用已有设备填充（编辑模式；会保留原设备 id）。
    void setDevice(const DeviceInfo &device);

    /// 读取界面上配置好的设备信息。
    DeviceInfo device() const;

private slots:
    void onProtocolChanged();

private:
    void setupUi();
    void applyProtocolVisibility();
    void loadPoints(const QList<TagPoint> &points);
    QList<TagPoint> collectPoints() const;

    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_protocolBox = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QSpinBox *m_slaveSpin = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QLineEdit *m_topicEdit = nullptr;
    QLabel *m_slaveLabel = nullptr;
    QLabel *m_topicLabel = nullptr;
    QTableWidget *m_pointTable = nullptr;

    DeviceInfo m_device; ///< 编辑模式下保留原 id
};
