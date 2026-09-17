#pragma once

#include <QWidget>

class DataStorage;
class DeviceManager;
class QDateTimeEdit;
class QLabel;
class QTableWidget;

/// 报表统计面板：按时间范围统计各设备的采样点数、告警次数与运行时长。
class ReportPanel : public QWidget
{
    Q_OBJECT

public:
    ReportPanel(DataStorage *storage, DeviceManager *manager, QWidget *parent = nullptr);

public slots:
    /// 按当前时间范围重新统计。
    void refresh();

private slots:
    void setRangeToday();
    void setRangeWeek();
    void setRangeMonth();

    /// 把当前时间范围的统计结果导出成 Excel（SpreadsheetML 2003，零第三方依赖）。
    void onExportExcel();

private:
    void setupUi();

    /// 设备名（找不到时回落到 id）—— 报表里给人看的必须是名字。
    QString deviceName(const QString &deviceId) const;

    DataStorage *m_storage = nullptr;
    DeviceManager *m_manager = nullptr;

    QDateTimeEdit *m_fromEdit = nullptr;
    QDateTimeEdit *m_toEdit = nullptr;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;
};
