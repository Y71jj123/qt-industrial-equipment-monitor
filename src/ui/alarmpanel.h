#pragma once

#include "core/alarmengine.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>

class DataStorage;
class DeviceManager;
class QCheckBox;
class QLabel;
class QTableWidget;

/// 告警面板：活动告警 + 历史的统一列表。
///
/// 只负责展示与转发用户操作，数据全部来自 AlarmEngine；
/// 支持单条处理（工单闭环）/ 全部确认 / 清空记录 / 只看活动告警。
///
/// 「确认」和「处理」是**两件事**，别混：
///   - 确认：我看见这条告警了（一次点击）；
///   - 处理：我写下了怎么解决的（要填结论，落库，计入 MTTR 统计）。
class AlarmPanel : public QWidget
{
    Q_OBJECT

public:
    AlarmPanel(AlarmEngine *engine,
               DeviceManager *manager,
               DataStorage *storage,
               const QString &currentUser,
               QWidget *parent = nullptr);

public slots:
    /// 从告警引擎重新拉取数据、刷新表格。
    void refresh();

private:
    void setupUi();
    void acknowledgeCurrent();

    /// 打开「录入处理结论」对话框，写回引擎并落库（工单闭环）。
    void handleCurrent();

    AlarmEngine *m_engine = nullptr;
    DeviceManager *m_manager = nullptr; ///< 仅用于把设备 id 显示成名称
    DataStorage *m_storage = nullptr;   ///< 把处理结论落库（可为空，仅影响持久化）
    QString m_currentUser;              ///< 处理人输入框的默认值

    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QCheckBox *m_activeOnly = nullptr;

    QList<QPair<QString, QString>> m_rowKeys; ///< 每行对应的 (deviceId, tagId)
};
