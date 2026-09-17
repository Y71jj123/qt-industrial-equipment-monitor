#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;

/// 用户角色。
enum class UserRole
{
    Operator = 0,     ///< 操作员：只读查看 + 确认告警
    Administrator = 1 ///< 管理员：可增删设备 / 改规则 / 下发指令
};

/// 角色的中文名。
QString userRoleName(UserRole role);

/// 登录对话框。
///
/// 演示用的本地账号（生产环境应换成服务端鉴权 + 密码哈希）：
///   - `admin / admin123`       管理员
///   - `operator / operator123` 操作员
class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget *parent = nullptr);

    QString userName() const { return m_userName; }
    UserRole role() const { return m_role; }

protected:
    void accept() override;

private:
    void setupUi();

    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLabel *m_hint = nullptr;

    QString m_userName;
    UserRole m_role = UserRole::Operator;
};
