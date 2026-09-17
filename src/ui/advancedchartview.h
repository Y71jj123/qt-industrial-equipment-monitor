#pragma once

#include <QColor>
#include <QDateTime>
#include <QPointF>
#include <QStringList>
#include <QtCharts/QChartView>

class QDateTimeAxis;
class QLineSeries;
class QValueAxis;

/// 可交互的趋势图视图：滚轮缩放 / 拖拽平移 / 右键复位 / 悬停读数。
///
/// QChartView 只负责"把图显示出来"，上面这四件事都得自己接鼠标事件。
/// 实时趋势与历史查询两个图的需求完全一样，所以做成一个可复用的视图类，
/// 两边各自只管把数据塞进 series。
///
/// **自动跟随**是这里最关键的一个状态：实时趋势的数据是滚动进来的，
/// 宿主每来一个点就会重设横轴范围 —— 用户一旦手动缩放 / 平移，
/// 这个"跟随"必须先关掉，否则下一拍（100ms 后）视图就被拽回原样，
/// 缩放等于没做。右键复位会重新打开跟随，并通知宿主恢复基准范围。
///
/// 横轴固定按 QDateTimeAxis 处理、纵轴固定按 QValueAxis：
/// 本项目两个图都是"时间 - 数值"，为此抽象一层轴接口并不划算。
class AdvancedChartView : public QChartView
{
    Q_OBJECT

public:
    explicit AdvancedChartView(QChart *chart, QWidget *parent = nullptr);

    /// 设定"右键复位"要回到的范围。宿主每次重设基准范围时调用
    /// （实时趋势每拍调一次，历史查询在查询完成后调一次）。
    void setHomeRange(const QDateTime &from, const QDateTime &to, double yMin, double yMax);

    /// 是否处于"跟随最新数据"状态。手动缩放 / 平移会自动关掉它。
    bool autoFollow() const { return m_autoFollow; }
    void setAutoFollow(bool on);

    /// 十字光标颜色。图表配色不归 QSS 管，只能由主题刷新时一并设置。
    void setCrosshairColor(const QColor &color);

signals:
    /// 自动跟随状态变化（用户开始手动操作 → false；右键复位 → true）。
    void autoFollowChanged(bool on);

    /// 用户右键复位：宿主应重新设定基准范围。
    void homeRequested();

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    /// 鼠标移出视口时收起十字光标 —— QAbstractScrollArea 会把视口事件转到这里，
    /// 而 QWidget::leaveEvent 收到的是"离开滚动区"而不是"离开视口"，不保险。
    bool viewportEvent(QEvent *event) override;

private:
    QDateTimeAxis *timeAxis() const;
    QValueAxis *valueAxis() const;

    /// 场景坐标 → 数据坐标。自己按坐标轴范围换算，不去用 QChart::mapToValue ——
    /// 那个函数在多条 series 共享坐标轴时的取值域不直观，换算公式反而更清楚。
    bool valueAt(const QPointF &scenePos, double *x, double *y) const;

    /// 以 @p scenePos 为锚点缩放 @p factor 倍（>1 放大）。@p vertical 为真时缩放纵轴。
    void zoomAt(const QPointF &scenePos, double factor, bool vertical);

    void resetToHome();
    void updateHover(const QPointF &scenePos, const QPoint &globalPos);
    void clearHover();

    /// 找出 @p series 上横坐标最接近 @p xValue 的点下标。
    static QPointF nearestPoint(const QLineSeries *series, double xValue);

    /// 当前光标横坐标对应的各条曲线读数，拼成提示文本。
    QString hoverTextAt(double xValue) const;

    QDateTime m_homeFrom;
    QDateTime m_homeTo;
    double m_homeYMin = 0.0;
    double m_homeYMax = 0.0;

    bool m_autoFollow = true;
    bool m_dragging = false;
    QPointF m_dragPos;

    bool m_hoverVisible = false;
    QPointF m_hoverPos;

    QColor m_crosshairColor = QColor(0x88, 0x99, 0xaa);

    /// 单次缩放的比例下限 / 上限，防止一路滚到纳秒级或跨到几十年。
    static constexpr double kMinSpanMs = 500.0;
    static constexpr double kMaxSpanMs = 90.0 * 24 * 3600 * 1000;
};
