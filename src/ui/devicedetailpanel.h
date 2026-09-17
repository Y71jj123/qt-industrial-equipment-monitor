#pragma once

#include <QHash>
#include <QString>
#include <QVariant>
#include <QWidget>

class AcquisitionScheduler;
class DeviceManager;
class QLabel;
class QTableWidget;

/// 设备详情面板：显示选中设备的基本信息、采集状态与各点位当前值。
///
/// 数据来自 DeviceManager（设备台账）+ AcquisitionScheduler（是否在采集），
/// 采样值由主窗口转发进来，本面板只负责展示。
class DeviceDetailPanel : public QWidget
{
    Q_OBJECT

public:
    DeviceDetailPanel(DeviceManager *manager,
                      AcquisitionScheduler *scheduler,
                      QWidget *parent = nullptr);

public slots:
    /// 切换当前展示的设备。
    void setDevice(const QString &deviceId);

    /// 收到新采样值时更新对应单元格（只处理当前正在展示的设备）。
    void updateValue(const QString &deviceId, const QString &tagId, const QVariant &value);

    /// 重新读取设备信息（在线 / 采集状态变化时调用）。
    void refresh();

private:
    void setupUi();

    DeviceManager *m_manager = nullptr;
    AcquisitionScheduler *m_scheduler = nullptr;
    QString m_deviceId;

    QLabel *m_title = nullptr;
    QLabel *m_protocol = nullptr;
    QLabel *m_endpoint = nullptr;
    QLabel *m_interval = nullptr;
    QLabel *m_state = nullptr;
    QTableWidget *m_pointTable = nullptr;
    QHash<QString, int> m_rowIndex; ///< tagId -> 表格行号
};
