#pragma once

#include <QHash>
#include <QString>
#include <QVariant>
#include <QWidget>

class AcquisitionScheduler;
class DataStorage;
class DeviceManager;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;

/// 远程控制面板。
///
/// 向指定设备的点位下发指令 —— 必须经过**二次确认**，并把每次操作
/// （谁、什么时候、对哪台设备哪个点位、改了什么、成功与否）写入操作留痕表。
class ControlPanel : public QWidget
{
    Q_OBJECT

public:
    ControlPanel(AcquisitionScheduler *scheduler,
                 DeviceManager *manager,
                 DataStorage *storage,
                 QWidget *parent = nullptr);

    /// 设置当前登录用户名（用于操作留痕）。
    void setUser(const QString &user);

public slots:
    /// 设备列表变化后刷新。
    void reload();

    /// 把采集到的最新值同步到"当前值"显示。
    void updateCurrentValue(const QString &deviceId, const QString &tagId, const QVariant &value);

    /// 刷新操作留痕表。
    void refreshLog();

private slots:
    void onDeviceChanged();
    void onSend();

private:
    void setupUi();
    void populatePoints(const QString &deviceId);

    AcquisitionScheduler *m_scheduler = nullptr;
    DeviceManager *m_manager = nullptr;
    DataStorage *m_storage = nullptr;
    QString m_user;

    QComboBox *m_deviceBox = nullptr;
    QComboBox *m_tagBox = nullptr;
    QDoubleSpinBox *m_valueSpin = nullptr;
    QLabel *m_currentLabel = nullptr;
    QTableWidget *m_logTable = nullptr;

    QHash<QString, double> m_currentValues; ///< key = deviceId/tagId
};
