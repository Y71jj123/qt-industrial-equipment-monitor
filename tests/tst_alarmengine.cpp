#include <QtTest>

#include "core/alarmengine.h"

/// AlarmEngine：状态机、分级判定、工单闭环与 MTTR。
///
/// 这些是**纯逻辑**，不碰界面也不碰数据库，所以用 QTEST_GUILESS_MAIN
/// （QCoreApplication）即可 —— CI 上没有显示器也能跑。
class TestAlarmEngine : public QObject
{
    Q_OBJECT

private slots:
    void riseAndFall();
    void noRepeatWhileActive();
    void acknowledgeIsNotHandle();
    void handleRecordsConclusion();
    void handleSyncsHistory();
    void rejectedWhenAlreadyCleared();
    void mttrWithoutSamples();
    void mttrAfterHandle();
    void levelByOverRange();
    void offlineAlarmIsSingle();

private:
    static AlarmRule rangeRule(const QString &deviceId, const QString &tagId,
                               double low, double high);
};

AlarmRule TestAlarmEngine::rangeRule(const QString &deviceId, const QString &tagId,
                                     double low, double high)
{
    AlarmRule rule;
    rule.deviceId = deviceId;
    rule.tagId = tagId;
    rule.kind = AlarmKind::Range;
    rule.lowLimit = low;
    rule.highLimit = high;
    return rule;
}

void TestAlarmEngine::riseAndFall()
{
    AlarmEngine engine;
    engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 10.0));

    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 50.0);
    QCOMPARE(int(engine.activeAlarms().size()), 1);

    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 5.0);
    QCOMPARE(int(engine.activeAlarms().size()), 0);
    QVERIFY(!engine.history().isEmpty());
}

void TestAlarmEngine::noRepeatWhileActive()
{
    // 同一点位在告警未消除前反复越限，只能有一条 —— 否则列表会被刷屏淹没。
    AlarmEngine engine;
    engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 10.0));

    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 50.0);
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 60.0);
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 70.0);
    QCOMPARE(int(engine.activeAlarms().size()), 1);
}

void TestAlarmEngine::acknowledgeIsNotHandle()
{
    AlarmEngine engine;
    engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 10.0));
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 50.0);

    engine.acknowledge(QStringLiteral("d"), QStringLiteral("t"));

    const QList<AlarmRecord> active = engine.activeAlarms();
    QCOMPARE(int(active.size()), 1);
    QVERIFY(active.at(0).acknowledged);
    // 核心语义：「确认」= 看见了，「处理」= 写了结论。两者不能混为一谈。
    QVERIFY(!active.at(0).handled());
    QCOMPARE(engine.handledCount(), 0);
}

void TestAlarmEngine::handleRecordsConclusion()
{
    AlarmEngine engine;
    engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 10.0));
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 50.0);

    QVERIFY(engine.handleAlarm(QStringLiteral("d"), QStringLiteral("t"),
                               AlarmDisposition::Maintained, QStringLiteral("张三"),
                               QStringLiteral("更换 3 号轴承")));

    const AlarmRecord record = engine.activeAlarms().at(0);
    QVERIFY(record.handled());
    QVERIFY(record.acknowledged); // 写了结论自然算已确认
    QCOMPARE(int(record.disposition), int(AlarmDisposition::Maintained));
    QCOMPARE(record.handledBy, QStringLiteral("张三"));
    QCOMPARE(record.handlingNote, QStringLiteral("更换 3 号轴承"));
    QCOMPARE(engine.handledCount(), 1);
}

void TestAlarmEngine::handleSyncsHistory()
{
    // 活动集合与历史两份数据必须一起写：只改一边，面板和历史就会对不上。
    AlarmEngine engine;
    engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 10.0));
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 50.0);
    engine.handleAlarm(QStringLiteral("d"), QStringLiteral("t"), AlarmDisposition::Resolved,
                       QStringLiteral("李四"), QStringLiteral("复位后正常"));

    bool synced = false;
    for (const AlarmRecord &item : engine.history()) {
        if (item.deviceId == QLatin1String("d") && item.tagId == QLatin1String("t")
            && item.handled()) {
            synced = true;
        }
    }
    QVERIFY(synced);
}

void TestAlarmEngine::rejectedWhenAlreadyCleared()
{
    // 已恢复的记录属于"事后补录"，不在本功能范围内，必须明确拒绝。
    AlarmEngine engine;
    engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 10.0));
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 50.0);
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 5.0);

    QVERIFY(!engine.handleAlarm(QStringLiteral("d"), QStringLiteral("t"),
                                AlarmDisposition::Resolved, QStringLiteral("王五"),
                                QStringLiteral("事后补录")));
}

void TestAlarmEngine::mttrWithoutSamples()
{
    // 没有样本时必须返回 -1（界面显示 "—"），不能返回 0 假装"处理得飞快"。
    AlarmEngine engine;
    QCOMPARE(engine.averageHandleDurationMs(), qint64(-1));
    QCOMPARE(engine.handledCount(), 0);
}

void TestAlarmEngine::mttrAfterHandle()
{
    AlarmEngine engine;
    engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 10.0));
    engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), 50.0);
    engine.handleAlarm(QStringLiteral("d"), QStringLiteral("t"), AlarmDisposition::Resolved,
                       QStringLiteral("李四"), QStringLiteral("复位"));

    const qint64 mttr = engine.averageHandleDurationMs();
    QVERIFY2(mttr >= 0, "有样本时 MTTR 不应为负");
    QVERIFY2(mttr < 5000, "告警产生后立刻处理，MTTR 应该接近 0");
}

void TestAlarmEngine::levelByOverRange()
{
    // 量程 [0,100]：越限 1% → 提示，5% → 警告，20% → 严重
    const auto levelOf = [](double value) {
        AlarmEngine engine;
        engine.addRule(rangeRule(QStringLiteral("d"), QStringLiteral("t"), 0.0, 100.0));
        engine.checkValue(QStringLiteral("d"), QStringLiteral("t"), value);
        return int(engine.activeAlarms().at(0).level);
    };

    QCOMPARE(levelOf(101.0), int(AlarmLevel::Info));
    QCOMPARE(levelOf(105.0), int(AlarmLevel::Warning));
    QCOMPARE(levelOf(120.0), int(AlarmLevel::Critical));
}

void TestAlarmEngine::offlineAlarmIsSingle()
{
    AlarmEngine engine;
    engine.raiseOfflineAlarm(QStringLiteral("d"), QStringLiteral("1 号空压机"));
    QCOMPARE(int(engine.activeAlarms().size()), 1);
    QCOMPARE(int(engine.activeAlarms().at(0).kind), int(AlarmKind::Offline));

    // 重复上报离线不能刷出多条
    engine.raiseOfflineAlarm(QStringLiteral("d"), QStringLiteral("1 号空压机"));
    QCOMPARE(int(engine.activeAlarms().size()), 1);

    engine.clearOfflineAlarm(QStringLiteral("d"));
    QCOMPARE(int(engine.activeAlarms().size()), 0);
}

QTEST_GUILESS_MAIN(TestAlarmEngine)

#include "tst_alarmengine.moc"
