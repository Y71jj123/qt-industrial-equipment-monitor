#pragma once

#include <QDateTime>
#include <QWidget>

class DataStorage;
class DeviceManager;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QTableWidget;

#ifdef HAVE_QT_CHARTS
class QChart;
class QChartView;
class QDateTimeAxis;
class QLineSeries;
class QValueAxis;
#endif

/// 历史数据查询面板。
///
/// 按「设备 + 点位 + 时间范围」从 SQLite 查回历史采样，表格 + 曲线双视图，
/// 并支持导出 CSV。数据是采集时一直在写的那份 —— 这里把它读出来。
class HistoryPanel : public QWidget
{
    Q_OBJECT

public:
    HistoryPanel(DataStorage *storage, DeviceManager *manager, QWidget *parent = nullptr);

    /// 设备列表变化时刷新设备下拉。
    void reloadDevices();

private slots:
    void onDeviceChanged();
    void onQuery();
    void onExportCsv();
    void setRangeLastHour();
    void setRangeToday();

private:
    void setupUi();
    void populatePoints(const QString &deviceId);

    DataStorage *m_storage = nullptr;
    DeviceManager *m_manager = nullptr;

    QComboBox *m_deviceBox = nullptr;
    QComboBox *m_tagBox = nullptr;
    QDateTimeEdit *m_fromEdit = nullptr;
    QDateTimeEdit *m_toEdit = nullptr;
    QLabel *m_summary = nullptr;
    QTableWidget *m_table = nullptr;

#ifdef HAVE_QT_CHARTS
    QChart *m_chart = nullptr;
    QChartView *m_chartView = nullptr;
    QDateTimeAxis *m_axisX = nullptr;
    QValueAxis *m_axisY = nullptr;
    QLineSeries *m_series = nullptr;
#endif
};
