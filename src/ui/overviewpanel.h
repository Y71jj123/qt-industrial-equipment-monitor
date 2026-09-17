#pragma once

#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <QWidget>

class AcquisitionScheduler;
class AlarmEngine;
class DataStorage;
class DeviceManager;
class QFrame;
class QGridLayout;
class QHideEvent;
class QLabel;
class QShowEvent;
class QTimer;

/// 总览仪表盘（首页）。
///
/// 面向"一眼看全局"这个诉求：
///   - 上半部分是一堵 KPI 数字墙（设备 / 在线率 / 告警 / 数据量），
///     每张卡片左侧一道色条区分语义，数值用大字号承担视觉重心；
///   - 下半部分是每台设备一张状态小卡（状态灯 + 关键点位实时值 + 活动告警数），
///     哪台在闹、哪台断了，不用点进去就能看出来。
///
/// 刷新策略分三路，避免"页面没开也在狂刷"：
///   - 设备 / 告警状态变化 → 信号驱动，立刻刷
///   - 采样数据到达 → 攒 250ms 一批只更新对应设备卡（不重建控件）
///   - 数据库里的累计统计 → 低频定时器，且**只在页面可见时**才转
class OverviewPanel : public QWidget
{
    Q_OBJECT

public:
    OverviewPanel(DeviceManager *deviceManager,
                  AlarmEngine *alarmEngine,
                  DataStorage *storage,
                  AcquisitionScheduler *scheduler,
                  QWidget *parent = nullptr);

public slots:
    /// 整页重算 + 重建设备卡（设备增删、分组变化、切到本页时调用）。
    void refresh();

signals:
    /// 用户点了某台设备的状态卡 —— 上层据此在设备树里选中它并跳到实时数据。
    void deviceActivated(const QString &deviceId);

protected:
    /// 页面可见才刷新：仪表盘的统计查询没必要在后台陪跑。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    /// 让设备卡可点：卡片是普通 QFrame，用事件过滤器补上"点击"语义，
    /// 比为一张卡片单独派生一个类划算得多。
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    /// 采集数据到达：只记值 + 标脏，真正的界面更新由定时器合并。
    void onTagUpdated(const QString &deviceId, const QString &tagId, const QVariant &value);

    /// 攒批刷新被标脏的设备卡。
    void flushDirtyDeviceCards();

    /// 重算来自数据库的累计统计（全量 / 今日采样点）。
    void refreshDatabaseStats();

private:
    /// 一张 KPI 卡：标题 + 大数字 + 单位 + 脚注。
    struct KpiCard
    {
        QFrame *frame = nullptr;
        QLabel *value = nullptr;
        QLabel *hint = nullptr;
    };

    /// 一张设备状态小卡。
    struct DeviceCard
    {
        QFrame *frame = nullptr;
        QLabel *light = nullptr;
        QLabel *name = nullptr;
        QLabel *value = nullptr;
        QLabel *meta = nullptr;
        QString primaryTag; ///< 卡面大数字对应的点位 id（空 = 该设备没有可显示的点位）
        QString primaryUnit;
    };

    void setupUi();

    /// 建一张 KPI 卡并挂到 @p row / @p column 上。
    KpiCard *createKpiCard(int row, int column, const QString &title, const QString &unit);

    /// 切换卡片的语义配色（tone 取 "" / "Ok" / "Warn" / "Danger"）。
    /// 名称没变时不触发重绘 —— 每次都 unpolish 一遍纯属白费。
    static void applyTone(KpiCard *card, const QString &tone);

    void rebuildDeviceCards();
    void updateKpiValues();
    void updateDeviceCard(const QString &deviceId);
    void markAllDevicesDirty();

    static QString valueKey(const QString &deviceId, const QString &tagId);

    DeviceManager *m_deviceManager = nullptr;
    AlarmEngine *m_alarmEngine = nullptr;
    DataStorage *m_storage = nullptr;
    AcquisitionScheduler *m_scheduler = nullptr;

    KpiCard *m_cardDevices = nullptr;
    KpiCard *m_cardOnline = nullptr;
    KpiCard *m_cardOffline = nullptr;
    KpiCard *m_cardOnlineRate = nullptr;
    KpiCard *m_cardActiveAlarms = nullptr;
    KpiCard *m_cardUnackAlarms = nullptr;
    KpiCard *m_cardTotalSamples = nullptr;
    KpiCard *m_cardSessionSamples = nullptr;

    QGridLayout *m_kpiGrid = nullptr;
    QGridLayout *m_deviceGrid = nullptr;
    QLabel *m_deviceSectionTitle = nullptr;
    QLabel *m_emptyHint = nullptr;

    QTimer *m_statsTimer = nullptr;  ///< 数据库统计（低频，页面可见才转）
    QTimer *m_cardTimer = nullptr;   ///< 设备卡攒批刷新（单次触发）

    QHash<QString, DeviceCard> m_deviceCards;
    QSet<QString> m_dirtyDevices;

    QHash<QString, QVariant> m_lastValues;  ///< deviceId/tagId -> 最近值
    QHash<QString, QDateTime> m_lastSeen;   ///< deviceId -> 最近收到数据的时间

    qint64 m_sessionSamples = 0;   ///< 本次运行收到的采样点（不依赖数据库，实时）
    qint64 m_totalSamples = 0;     ///< 数据库里的累计采样点
    qint64 m_todaySamples = 0;     ///< 今日采样点
};
