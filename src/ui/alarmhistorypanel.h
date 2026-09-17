#pragma once

#include "core/alarmengine.h"

#include <QList>
#include <QString>
#include <QWidget>

class DataStorage;
class DeviceManager;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QTableWidget;

/// 报警历史查询面板。
///
/// 与「告警」页的分工：那边看的是**当前**告警（活动 / 未确认，来自 AlarmEngine 的内存态），
/// 这边查的是**库里的历史**（DataStorage::queryAlarms），按时间 / 级别 / 设备任意组合筛选，
/// 结果可排序、可导出 CSV —— 事故复盘和交班记录都靠它。
///
/// 刻意不做自动刷新：查询条件是用户自己定的，数据在他看的过程中跳来跳去反而没法核对。
class AlarmHistoryPanel : public QWidget
{
    Q_OBJECT

public:
    AlarmHistoryPanel(DataStorage *storage, DeviceManager *manager, QWidget *parent = nullptr);

    /// 设备台账变化时刷新设备下拉。
    void reloadDevices();

private slots:
    void onQuery();
    void onExportCsv();
    void setRangeToday();
    void setRangeLastWeek();

private:
    void setupUi();

    /// 按当前的级别 / 设备筛选条件，把 m_records 过滤进 m_visible 并填表。
    void applyFilter();

    /// 把查询结果里出现过、但台账里已经没有的设备补进下拉（否则它的历史筛不出来）。
    void appendMissingDevices(const QList<AlarmRecord> &records);

    /// 把表格当前展示的内容写成 CSV；失败原因见 lastExportError()。
    bool exportCsv(const QString &path);

    QString deviceName(const QString &deviceId) const;

    DataStorage *m_storage = nullptr;
    DeviceManager *m_manager = nullptr;

    QComboBox *m_deviceBox = nullptr;
    QComboBox *m_levelBox = nullptr;
    QDateTimeEdit *m_fromEdit = nullptr;
    QDateTimeEdit *m_toEdit = nullptr;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;

    QList<AlarmRecord> m_records; ///< 最近一次查询的原始结果
    QList<AlarmRecord> m_visible; ///< 表格正在展示的行（导出的就是这些）

    QString m_exportError;
};
