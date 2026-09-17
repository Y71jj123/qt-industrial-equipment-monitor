#include "ui/advancedchartview.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <QtCharts/QAbstractAxis>
#include <QtCharts/QChart>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <cmath>

namespace {

/// 每格滚轮（120）的缩放倍率，手感接近地图软件。
constexpr double kZoomPerNotch = 1.2;

/// 拖拽平移的像素阈值：低于它视为"点一下"，免得手抖把曲线推歪。
constexpr qreal kDragThresholdPx = 3.0;

} // namespace

AdvancedChartView::AdvancedChartView(QChart *chart, QWidget *parent)
    : QChartView(chart, parent)
{
    setRenderHint(QPainter::Antialiasing);
    setMouseTracking(true); // 不按键也要收 mouseMove，否则悬停读数无从谈起
    viewport()->setMouseTracking(true);
    setCursor(Qt::CrossCursor); // 光标本身就暗示"这里可以读点、可以拖"
}

QDateTimeAxis *AdvancedChartView::timeAxis() const
{
    const QList<QAbstractAxis *> axes = chart()->axes(Qt::Horizontal);
    for (QAbstractAxis *axis : axes) {
        if (auto *time = qobject_cast<QDateTimeAxis *>(axis))
            return time;
    }
    return nullptr;
}

QValueAxis *AdvancedChartView::valueAxis() const
{
    const QList<QAbstractAxis *> axes = chart()->axes(Qt::Vertical);
    for (QAbstractAxis *axis : axes) {
        if (auto *value = qobject_cast<QValueAxis *>(axis))
            return value;
    }
    return nullptr;
}

void AdvancedChartView::setHomeRange(const QDateTime &from,
                                     const QDateTime &to,
                                     double yMin,
                                     double yMax)
{
    m_homeFrom = from;
    m_homeTo = to;
    m_homeYMin = yMin;
    m_homeYMax = yMax;
}

void AdvancedChartView::setAutoFollow(bool on)
{
    if (m_autoFollow == on)
        return;

    m_autoFollow = on;
    emit autoFollowChanged(on);
}

void AdvancedChartView::setCrosshairColor(const QColor &color)
{
    m_crosshairColor = color;
    if (m_hoverVisible)
        viewport()->update();
}

QPointF AdvancedChartView::nearestPoint(const QLineSeries *series, double xValue)
{
    const QList<QPointF> points = series->points();
    if (points.isEmpty())
        return QPointF();

    int nearest = 0;
    double bestDistance = std::abs(points.at(0).x() - xValue);
    for (int i = 1; i < points.size(); ++i) {
        const double distance = std::abs(points.at(i).x() - xValue);
        if (distance < bestDistance) {
            bestDistance = distance;
            nearest = i;
        }
    }
    return points.at(nearest);
}

bool AdvancedChartView::valueAt(const QPointF &scenePos, double *x, double *y) const
{
    QDateTimeAxis *axisX = timeAxis();
    QValueAxis *axisY = valueAxis();
    if (!axisX || !axisY)
        return false;

    const QRectF plot = chart()->plotArea();
    if (plot.width() <= 0.0 || plot.height() <= 0.0)
        return false;

    const double fromMs = double(axisX->min().toMSecsSinceEpoch());
    const double toMs = double(axisX->max().toMSecsSinceEpoch());
    const double loY = axisY->min();
    const double hiY = axisY->max();

    if (x)
        *x = fromMs + (scenePos.x() - plot.left()) / plot.width() * (toMs - fromMs);
    if (y)
        *y = hiY - (scenePos.y() - plot.top()) / plot.height() * (hiY - loY);
    return true;
}

void AdvancedChartView::zoomAt(const QPointF &scenePos, double factor, bool vertical)
{
    QDateTimeAxis *axisX = timeAxis();
    QValueAxis *axisY = valueAxis();
    if (!axisX || !axisY)
        return;

    double anchorX = 0.0;
    double anchorY = 0.0;
    if (!valueAt(scenePos, &anchorX, &anchorY))
        return;

    if (vertical) {
        const double lo = axisY->min();
        const double hi = axisY->max();
        const double span = hi - lo;
        if (span <= 0.0)
            return;

        axisY->setRange(anchorY - (anchorY - lo) / factor, anchorY + (hi - anchorY) / factor);
    } else {
        const double fromMs = double(axisX->min().toMSecsSinceEpoch());
        const double toMs = double(axisX->max().toMSecsSinceEpoch());
        const double span = toMs - fromMs;
        if (span <= 0.0)
            return;

        const double newSpan = qBound(kMinSpanMs, span / factor, kMaxSpanMs);
        const double ratio = (anchorX - fromMs) / span; // 锚点在原区间里的相对位置
        const double newFrom = anchorX - ratio * newSpan;

        axisX->setRange(QDateTime::fromMSecsSinceEpoch(qint64(newFrom)),
                        QDateTime::fromMSecsSinceEpoch(qint64(newFrom + newSpan)));
    }

    // 缩放了就说明用户在"看历史"，实时跟随必须让位
    setAutoFollow(false);
}

void AdvancedChartView::resetToHome()
{
    QDateTimeAxis *axisX = timeAxis();
    QValueAxis *axisY = valueAxis();

    if (axisX && m_homeFrom.isValid() && m_homeTo.isValid())
        axisX->setRange(m_homeFrom, m_homeTo);
    if (axisY && m_homeYMax > m_homeYMin)
        axisY->setRange(m_homeYMin, m_homeYMax);

    setAutoFollow(true);
    emit homeRequested();
}

void AdvancedChartView::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        QChartView::wheelEvent(event);
        return;
    }

    // Ctrl + 滚轮缩放纵轴：横轴是时间，缩放它才是浏览历史的主要诉求，
    // 纵轴只在"看不清波形幅度"时偶尔调一下，所以放在修饰键上、不抢默认手势。
    const bool vertical = event->modifiers().testFlag(Qt::ControlModifier);
    const double factor = std::pow(kZoomPerNotch, delta / 120.0);

    zoomAt(mapToScene(event->position().toPoint()), factor, vertical);
    clearHover(); // 缩放后原来的读数位置已经不对了
    event->accept();
}

void AdvancedChartView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton) {
        resetToHome();
        clearHover();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragPos = mapToScene(event->position().toPoint());
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    QChartView::mousePressEvent(event);
}

void AdvancedChartView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF scenePos = mapToScene(event->position().toPoint());

    if (m_dragging) {
        const QPointF delta = scenePos - m_dragPos;
        if (delta.manhattanLength() >= kDragThresholdPx) {
            QDateTimeAxis *axisX = timeAxis();
            QValueAxis *axisY = valueAxis();
            const QRectF plot = chart()->plotArea();

            if (axisX && axisY && plot.width() > 0.0 && plot.height() > 0.0) {
                const double fromMs = double(axisX->min().toMSecsSinceEpoch());
                const double toMs = double(axisX->max().toMSecsSinceEpoch());
                const double loY = axisY->min();
                const double hiY = axisY->max();

                // 往右拖 = 看更早的数据，所以横轴反向移动；纵轴同理（场景 y 向下为正）
                const double shiftX = -delta.x() / plot.width() * (toMs - fromMs);
                const double shiftY = delta.y() / plot.height() * (hiY - loY);

                axisX->setRange(QDateTime::fromMSecsSinceEpoch(qint64(fromMs + shiftX)),
                                QDateTime::fromMSecsSinceEpoch(qint64(toMs + shiftX)));
                axisY->setRange(loY + shiftY, hiY + shiftY);

                m_dragPos = scenePos;
                setAutoFollow(false);
                m_hoverVisible = false; // 拖动时读数会跟着乱跳，先收起来
            }
        }
        event->accept();
        return;
    }

    updateHover(scenePos, event->globalPosition().toPoint());
    QChartView::mouseMoveEvent(event);
}

void AdvancedChartView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::CrossCursor);
        event->accept();
        return;
    }

    QChartView::mouseReleaseEvent(event);
}

bool AdvancedChartView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave)
        clearHover();

    return QChartView::viewportEvent(event);
}

void AdvancedChartView::updateHover(const QPointF &scenePos, const QPoint &globalPos)
{
    double xValue = 0.0;
    if (!valueAt(scenePos, &xValue, nullptr) || !chart()->plotArea().contains(scenePos)) {
        clearHover();
        return;
    }

    m_hoverPos = scenePos;
    m_hoverVisible = true;
    viewport()->update();

    const QString text = hoverTextAt(xValue);
    if (text.isEmpty())
        QToolTip::hideText();
    else
        QToolTip::showText(globalPos, text, this);
}

void AdvancedChartView::clearHover()
{
    if (!m_hoverVisible)
        return;

    m_hoverVisible = false;
    QToolTip::hideText();
    viewport()->update();
}

QString AdvancedChartView::hoverTextAt(double xValue) const
{
    QStringList lines;
    const QList<QAbstractSeries *> seriesList = chart()->series();
    for (QAbstractSeries *abstract : seriesList) {
        auto *series = qobject_cast<QLineSeries *>(abstract);
        if (!series || series->count() == 0)
            continue;

        const QPointF point = nearestPoint(series, xValue);
        const QString name = series->name().isEmpty() ? QStringLiteral("数值") : series->name();
        const QString time = QDateTime::fromMSecsSinceEpoch(qint64(point.x()))
                                 .toString(QStringLiteral("HH:mm:ss"));
        lines.append(QStringLiteral("%1：%2   %3")
                         .arg(name)
                         .arg(point.y(), 0, 'f', 2)
                         .arg(time));
    }

    return lines.join(QLatin1Char('\n'));
}

void AdvancedChartView::paintEvent(QPaintEvent *event)
{
    QChartView::paintEvent(event);

    if (!m_hoverVisible || !chart())
        return;

    QDateTimeAxis *axisX = timeAxis();
    QValueAxis *axisY = valueAxis();
    if (!axisX || !axisY)
        return;

    const double fromMs = double(axisX->min().toMSecsSinceEpoch());
    const double toMs = double(axisX->max().toMSecsSinceEpoch());
    const double loY = axisY->min();
    const double hiY = axisY->max();
    if (toMs <= fromMs || hiY <= loY)
        return;

    // 绘图区在场景坐标里，而 QPainter 画的是视口 —— 视口被缩得比图表最小尺寸还小时
    // 两者会差一个偏移，所以统一转一次，别假设 scene == viewport。
    const QRectF plot = chart()->plotArea();
    const QRectF viewPlot = mapFromScene(plot).boundingRect();
    if (viewPlot.isEmpty())
        return;

    // 在基类画完之后叠一层：十字光标属于"浮在图表上的装饰"，
    // 塞进 QChart 的图元体系反而要处理与坐标轴的层级关系，不划算。
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);

    QPen pen(m_crosshairColor);
    pen.setStyle(Qt::DashLine);
    pen.setWidth(1);
    painter.setPen(pen);

    const QPointF cursor = mapFromScene(m_hoverPos);
    painter.drawLine(QPointF(cursor.x(), viewPlot.top()), QPointF(cursor.x(), viewPlot.bottom()));
    painter.drawLine(QPointF(viewPlot.left(), cursor.y()), QPointF(viewPlot.right(), cursor.y()));

    // 每条曲线上标出被读到的那一点，读数与曲线才对得上号
    painter.setPen(Qt::NoPen);
    const QList<QAbstractSeries *> seriesList = chart()->series();
    for (QAbstractSeries *abstract : seriesList) {
        auto *series = qobject_cast<QLineSeries *>(abstract);
        if (!series || series->count() == 0)
            continue;

        double xValue = 0.0;
        if (!valueAt(m_hoverPos, &xValue, nullptr))
            continue;

        const QPointF point = nearestPoint(series, xValue);
        const QPointF scenePoint(plot.left() + (point.x() - fromMs) / (toMs - fromMs) * plot.width(),
                                 plot.top() + (hiY - point.y()) / (hiY - loY) * plot.height());

        painter.setBrush(series->color());
        painter.drawEllipse(QPointF(mapFromScene(scenePoint)), 3.5, 3.5);
    }
}
