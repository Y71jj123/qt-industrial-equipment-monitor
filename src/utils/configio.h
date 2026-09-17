#pragma once

#include "core/alarmengine.h"
#include "core/devicemanager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

/// 配置（设备台账 + 分组 + 告警规则）与 JSON 之间的互转。
///
/// 之所以单独放在工具层而不是塞进界面：点位表的 JSON 表示是**持久化格式**，
/// 数据库的 devices.points 列和导出的配置文件用的是同一份定义 ——
/// 两处各写一遍，早晚会漂移成"导出的文件导入不回来"。
///
/// 配置文件刻意不含历史采样与告警记录：那是数据不是配置，
/// 混在一起会让"导入配置"变成一个危险的删库操作。
namespace ConfigIo {

/// 当前配置文件格式版本。读到更高版本的文件会被拒绝 ——
/// 宁可报错，也不要按老规则误读将来才有的字段。
constexpr int kFormatVersion = 1;

/// 点位表 → JSON 数组（数据库列与配置文件共用）。
QJsonArray pointsToJson(const QList<TagPoint> &points);

/// JSON 数组 → 点位表；格式不对时返回空表。
QList<TagPoint> pointsFromJson(const QJsonArray &array);

/// 一份完整的配置。
struct Config
{
    QList<DeviceInfo> devices;
    QStringList groups;
    QList<AlarmRule> rules;
};

QJsonObject toJson(const QList<DeviceInfo> &devices,
                   const QStringList &groups,
                   const QList<AlarmRule> &rules);

/// 解析配置对象。失败时返回 false，并把中文原因写进 @p error。
bool fromJson(const QJsonObject &object, Config *config, QString *error);

/// 把配置写成带缩进的 JSON 文件（人可读、可手改，便于现场批量下发）。
bool saveToFile(const QString &filePath, const QJsonObject &object, QString *error);

/// 从文件读回 JSON 对象。
bool loadFromFile(const QString &filePath, QJsonObject *object, QString *error);

} // namespace ConfigIo
