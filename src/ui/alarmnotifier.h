#pragma once

#include "core/alarmengine.h"

#include <QColor>
#include <QObject>
#include <QString>
#include <QWidget>

class DeviceManager;

class QAction;
class QMenu;
class QSystemTrayIcon;
class QTimer;

/// 单条告警浮层（非模态通知条）。
///
/// 刻意不用 QMessageBox：模态框会打断操作，而工业现场告警往往是成批来的，
/// 一条条弹模态会把人锁死在对话框里。浮层只是"提醒"，点一下才跳转到告警列表。
class AlarmToast : public QWidget
{
    Q_OBJECT

public:
    AlarmToast(const AlarmRecord &record, const QString &deviceName, QWidget *parent = nullptr);

    /// 告警点位标识（deviceId/tagId）—— 同一位置只保留一条浮层，避免刷屏。
    QString key() const { return m_key; }
    bool critical() const { return m_record.level == AlarmLevel::Critical; }

    /// 挂上自动淡出计时。严重级别不参与，必须人工关掉。
    void startLifecycle();

signals:
    void dismissed(AlarmToast *toast);
    void activated(const QString &deviceId, const QString &tagId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    /// 动画淡出，结束后发 dismissed 由管理器回收。
    void fadeOut();

    AlarmRecord m_record;
    QString m_key;
    QColor m_accent;
    QTimer *m_timer = nullptr;
    bool m_fading = false;
};

/// 告警通知器：浮层 / 声音 / 系统托盘三件事都由它统一处理。
///
/// 三者都只在"有新告警"这一个时机动作，拆成三个类反而要各自重复订阅
/// AlarmEngine::alarmRaised，所以合成一个。
class AlarmNotifier : public QObject
{
    Q_OBJECT

public:
    AlarmNotifier(QWidget *host, AlarmEngine *engine, DeviceManager *manager);

    /// 声音报警开关；状态存在 QSettings 里，下次启动沿用。
    static bool soundEnabled();
    static void setSoundEnabled(bool enabled);

    /// 托盘是否可用（无托盘的环境下相关功能整体降级）。
    bool trayAvailable() const { return m_tray != nullptr; }

    /// 处理主窗口的关闭请求：托盘可用时收进托盘并返回 true，否则返回 false
    /// 让窗口正常关闭。没有托盘还把窗口藏起来的话，用户就再也叫不回来了。
    bool handleCloseRequest();

public slots:
    /// 新告警 → 浮层 + 声音 + 托盘气泡。
    void notify(const AlarmRecord &record);

    /// 把主窗口叫回前台（托盘菜单 / 双击托盘图标 / 点击浮层）。
    void showHost();

signals:
    /// 浮层被点击：界面据此跳到告警页。
    void alarmActivated(const QString &deviceId, const QString &tagId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupTray();
    void showToast(const AlarmRecord &record, const QString &deviceName);
    void removeToast(AlarmToast *toast);

    /// 浮层贴着宿主窗口右下角向上堆叠。
    void layoutToasts();

    void updateTrayIcon();
    void showTrayMessage(const AlarmRecord &record, const QString &deviceName);
    void quitApplication();

    QWidget *m_host = nullptr;
    AlarmEngine *m_engine = nullptr;
    DeviceManager *m_manager = nullptr;

    QList<AlarmToast *> m_toasts; ///< 最新在前

    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_trayMenu = nullptr;
    QAction *m_actionShow = nullptr;
    QAction *m_actionAckAll = nullptr;
    QAction *m_actionQuit = nullptr;

    bool m_quitting = false;       ///< 走的是"退出"而不是"关闭窗口"
    bool m_closeHintShown = false; ///< 关闭提示只弹一次，免得每次关都骚扰
};
