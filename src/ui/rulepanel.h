#pragma once

#include <QWidget>

class AlarmEngine;
class DeviceManager;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;

/// 告警规则配置面板：按「设备 + 点位」增删改告警规则。
///
/// 支持两种规则：
///   - 阈值：超出 [下限, 上限] 告警
///   - 变化率：单位时间变化量超过阈值告警
/// 替代原先写死在代码里的默认阈值。
class RulePanel : public QWidget
{
    Q_OBJECT

public:
    RulePanel(AlarmEngine *engine, DeviceManager *manager, QWidget *parent = nullptr);

public slots:
    /// 设备 / 规则变化后刷新设备下拉与规则表。
    void reload();

private slots:
    void onDeviceChanged();
    void onKindChanged();
    void onApply();
    void onRemoveSelected();
    void onTableSelectionChanged();

private:
    void setupUi();
    void populatePoints(const QString &deviceId);
    void refreshTable();

    AlarmEngine *m_engine = nullptr;
    DeviceManager *m_manager = nullptr;

    QComboBox *m_deviceBox = nullptr;
    QComboBox *m_tagBox = nullptr;
    QComboBox *m_kindBox = nullptr;
    QDoubleSpinBox *m_lowSpin = nullptr;
    QDoubleSpinBox *m_highSpin = nullptr;
    QDoubleSpinBox *m_rateSpin = nullptr;
    QCheckBox *m_enabledBox = nullptr;

    QLabel *m_lowLabel = nullptr;
    QLabel *m_highLabel = nullptr;
    QLabel *m_rateLabel = nullptr;

    QTableWidget *m_table = nullptr;
};
