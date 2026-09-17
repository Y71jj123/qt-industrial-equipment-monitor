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
/// 协议下拉的内容、以及"哪些字段该显示"全部由**协议注册表**驱动
/// （见 `IProtocolPlugin::traits()`）—— 所以外部协议插件装进来之后，
/// 这里的选项与字段会自动跟上，本文件里没有任何一个具体协议的名字。
class DeviceDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DeviceDialog(QWidget *parent = nullptr);

    /// 提供可选的分组列表。可编辑下拉：既能选已有的，也能直接敲一个新的。
    void setGroups(const QStringList &groups);

    /// 用已有设备填充（编辑模式；会保留原设备 id）。
    void setDevice(const DeviceInfo &device);

    /// 读取界面上配置好的设备信息。
    DeviceInfo device() const;

private slots:
    void onProtocolChanged();

private:
    void setupUi();
    void applyProtocolVisibility();
    void updateHint(const QString &protocolId);
    void loadPoints(const QList<TagPoint> &points);
    QList<TagPoint> collectPoints() const;

    /// 当前选中的协议 id（下拉框里存的就是它）。
    QString currentProtocolId() const;

    QLineEdit *m_nameEdit = nullptr;
    QComboBox *m_groupBox = nullptr;
    QComboBox *m_protocolBox = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QSpinBox *m_slaveSpin = nullptr;
    QSpinBox *m_intervalSpin = nullptr;
    QLineEdit *m_topicEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLabel *m_slaveLabel = nullptr;
    QLabel *m_topicLabel = nullptr;
    QLabel *m_userLabel = nullptr;
    QLabel *m_passwordLabel = nullptr;
    QLabel *m_hostLabel = nullptr;
    QLabel *m_portLabel = nullptr;
    QLabel *m_hintLabel = nullptr;
    QTableWidget *m_pointTable = nullptr;

    DeviceInfo m_device; ///< 编辑模式下保留原 id
};
