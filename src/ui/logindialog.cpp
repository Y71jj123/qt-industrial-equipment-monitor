#include "ui/logindialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

QString userRoleName(UserRole role)
{
    return role == UserRole::Administrator ? QStringLiteral("管理员") : QStringLiteral("操作员");
}

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
}

void LoginDialog::setupUi()
{
    setWindowTitle(QStringLiteral("登录"));
    setFixedWidth(400);

    auto *title = new QLabel(QStringLiteral("工业设备远程监控管理平台"), this);
    title->setObjectName(QStringLiteral("panelTitle"));
    title->setAlignment(Qt::AlignCenter);

    m_userEdit = new QLineEdit(this);
    m_userEdit->setPlaceholderText(QStringLiteral("用户名"));

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText(QStringLiteral("密码"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_hint = new QLabel(
        QStringLiteral("默认账号： admin / admin123（管理员）\n"
                       "                  operator / operator123（操作员）"),
        this);
    m_hint->setObjectName(QStringLiteral("panelHint"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("登录"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("退出"));

    connect(buttons, &QDialogButtonBox::accepted, this, &LoginDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("用户名"), m_userEdit);
    form->addRow(QStringLiteral("密码"), m_passwordEdit);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addSpacing(10);
    layout->addLayout(form);
    layout->addSpacing(4);
    layout->addWidget(m_hint);
    layout->addSpacing(8);
    layout->addWidget(buttons);
}

void LoginDialog::accept()
{
    const QString user = m_userEdit->text().trimmed();
    const QString password = m_passwordEdit->text();

    if (user == QStringLiteral("admin") && password == QStringLiteral("admin123")) {
        m_userName = user;
        m_role = UserRole::Administrator;
        QDialog::accept();
        return;
    }

    if (user == QStringLiteral("operator") && password == QStringLiteral("operator123")) {
        m_userName = user;
        m_role = UserRole::Operator;
        QDialog::accept();
        return;
    }

    QMessageBox::warning(this, QStringLiteral("登录失败"), QStringLiteral("用户名或密码不正确。"));
    m_passwordEdit->clear();
    m_passwordEdit->setFocus();
}
