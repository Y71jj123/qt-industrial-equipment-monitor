#pragma once

#include "core/alarmengine.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>

class DeviceManager;
class QCheckBox;
class QLabel;
class QTableWidget;

/// 告警面板：活动告警 + 历史的统一列表。
///
/// 只负责展示与转发用户操作，数据全部来自 AlarmEngine；
/// 支持单条确认 / 全部确认 / 清空记录 / 只看活动告警。
class AlarmPanel : public QWidget
{
    Q_OBJECT

public:
    AlarmPanel(AlarmEngine *engine, DeviceManager *manager, QWidget *parent = nullptr);

public slots:
    /// 从告警引擎重新拉取数据、刷新表格。
    void refresh();

private:
    void setupUi();
    void acknowledgeCurrent();

    AlarmEngine *m_engine = nullptr;
    DeviceManager *m_manager = nullptr; ///< 仅用于把设备 id 显示成名称

    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QCheckBox *m_activeOnly = nullptr;

    QList<QPair<QString, QString>> m_rowKeys; ///< 每行对应的 (deviceId, tagId)
};
