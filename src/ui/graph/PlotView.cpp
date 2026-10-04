// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/graph/PlotView.h"

#include "ui/theme/ThemeManager.h"

#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace torquebus::ui {
namespace {

constexpr double kLeftMargin = 68.0;
constexpr double kRightMargin = 16.0;
constexpr double kTopBaseMargin = 10.0;
constexpr double kTopTitleMargin = 32.0;
constexpr double kBottomBaseMargin = 24.0;
constexpr double kBottomDateMargin = 42.0;
constexpr double kSubplotGap = 12.0;

constexpr int kTimeDivisions = 6;
constexpr int kValueDivisions = 4;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

[[nodiscard]] QString formatSeconds(std::uint64_t nanoseconds, std::uint64_t spanNs)
{
    const double seconds = static_cast<double>(nanoseconds) / 1'000'000'000.0;
    const int decimals = spanNs < 2'000'000'000ULL ? 3 : (spanNs < 60'000'000'000ULL ? 2 : 1);
    return QStringLiteral("%1").arg(seconds, 0, 'f', decimals);
}

[[nodiscard]] QString
formatDateTime(const QDateTime& baseDt, std::uint64_t nanoseconds, std::uint64_t spanNs)
{
    const QDateTime pointDt = baseDt.addMSecs(static_cast<qint64>(nanoseconds / 1'000'000ULL));
    if (spanNs < 10'000'000'000ULL) {
        return pointDt.toString(QStringLiteral("hh:mm:ss.zzz"));
    }
    if (spanNs < 3600'000'000'000ULL) {
        return pointDt.toString(QStringLiteral("dd.MM.yyyy hh:mm:ss"));
    }
    return pointDt.toString(QStringLiteral("dd.MM.yyyy hh:mm"));
}

[[nodiscard]] QString formatValue(double value)
{
    const double magnitude = std::abs(value);
    if (magnitude >= 10'000.0) {
        return QStringLiteral("%1k").arg(value / 1000.0, 0, 'f', 1);
    }
    if (magnitude >= 100.0) {
        return QStringLiteral("%1").arg(value, 0, 'f', 0);
    }
    if (magnitude >= 1.0) {
        return QStringLiteral("%1").arg(value, 0, 'f', 1);
    }
    return QStringLiteral("%1").arg(value, 0, 'f', 3);
}

} // namespace

PlotView::PlotView(QWidget* parent)
    : QWidget{parent}
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

QSize PlotView::sizeHint() const
{
    return {680, 360};
}

QSize PlotView::minimumSizeHint() const
{
    return {260, 140};
}

void PlotView::setTraces(std::vector<PlotTrace> traces)
{
    // Preserve custom settings (muted, fillStyle, subplotIndex) from previous traces if matching
    for (PlotTrace& newTrace : traces) {
        for (const PlotTrace& oldTrace : m_traces) {
            if (newTrace.name == oldTrace.name) {
                newTrace.muted = oldTrace.muted;
                newTrace.fillStyle = oldTrace.fillStyle;
                newTrace.subplotIndex = oldTrace.subplotIndex;
                newTrace.lineWidth = oldTrace.lineWidth;
                break;
            }
        }
    }

    m_traces = std::move(traces);
    update();
}

void PlotView::setWindow(std::uint64_t startNs, std::uint64_t endNs)
{
    const std::uint64_t clampedEnd = std::max(endNs, startNs + 1'000'000ULL);
    if (m_startNs == startNs && m_endNs == clampedEnd) {
        return;
    }
    m_startNs = startNs;
    m_endNs = clampedEnd;

    // Keep measurement cursors within bounds
    if (m_cursorANs < m_startNs || m_cursorANs > m_endNs) {
        m_cursorANs = m_startNs + (m_endNs - m_startNs) * 2 / 10;
    }
    if (m_cursorBNs < m_startNs || m_cursorBNs > m_endNs) {
        m_cursorBNs = m_startNs + (m_endNs - m_startNs) * 8 / 10;
    }

    update();
}

void PlotView::setTitle(const QString& title)
{
    if (m_title == title) {
        return;
    }
    m_title = title;
    update();
}

void PlotView::setPlaceholder(const QString& text)
{
    if (m_placeholder == text) {
        return;
    }
    m_placeholder = text;
    update();
}

void PlotView::setToolMode(PlotToolMode mode)
{
    if (m_toolMode == mode) {
        return;
    }
    m_toolMode = mode;
    switch (mode) {
    case PlotToolMode::Pan:
        setCursor(Qt::OpenHandCursor);
        break;
    case PlotToolMode::ZoomBox:
        setCursor(Qt::CrossCursor);
        break;
    case PlotToolMode::DualCursor:
    case PlotToolMode::Pointer:
    default:
        setCursor(Qt::ArrowCursor);
        break;
    }
    update();
}

void PlotView::setSubplotLayoutMode(SubplotLayoutMode mode)
{
    if (m_layoutMode == mode) {
        return;
    }
    m_layoutMode = mode;
    update();
}

void PlotView::setTimeDisplayFormat(TimeDisplayFormat format)
{
    if (m_timeFormat == format) {
        return;
    }
    m_timeFormat = format;
    update();
}

void PlotView::setBaseDateTime(const QDateTime& dt)
{
    m_baseDateTime = dt;
    update();
}

void PlotView::setShowLegend(bool show)
{
    if (m_showLegend == show) {
        return;
    }
    m_showLegend = show;
    update();
}

void PlotView::setShowGrid(bool show)
{
    if (m_showGrid == show) {
        return;
    }
    m_showGrid = show;
    update();
}

void PlotView::setDualCursorsEnabled(bool enabled)
{
    if (m_dualCursors == enabled) {
        return;
    }
    m_dualCursors = enabled;
    if (enabled) {
        const std::uint64_t span = m_endNs - m_startNs;
        m_cursorANs = m_startNs + span * 25 / 100;
        m_cursorBNs = m_startNs + span * 75 / 100;
        Q_EMIT measurementChanged(m_cursorANs,
                                  m_cursorBNs,
                                  static_cast<qint64>(m_cursorBNs)
                                      - static_cast<qint64>(m_cursorANs));
    }
    update();
}

void PlotView::setCursorAPosition(std::uint64_t timestampNs)
{
    m_cursorANs = std::clamp(timestampNs, m_startNs, m_endNs);
    Q_EMIT measurementChanged(m_cursorANs,
                              m_cursorBNs,
                              static_cast<qint64>(m_cursorBNs) - static_cast<qint64>(m_cursorANs));
    update();
}

void PlotView::setCursorBPosition(std::uint64_t timestampNs)
{
    m_cursorBNs = std::clamp(timestampNs, m_startNs, m_endNs);
    Q_EMIT measurementChanged(m_cursorANs,
                              m_cursorBNs,
                              static_cast<qint64>(m_cursorBNs) - static_cast<qint64>(m_cursorANs));
    update();
}

int PlotView::subplotCount() const
{
    return static_cast<int>(computeSubplots().size());
}

double PlotView::valueAtTimestamp(const PlotTrace& trace, std::uint64_t timestampNs) const
{
    if (trace.samples.empty()) {
        return 0.0;
    }
    if (timestampNs == 0 || timestampNs >= trace.samples.back().timestampNs) {
        return trace.samples.back().value;
    }
    if (timestampNs <= trace.samples.front().timestampNs) {
        return trace.samples.front().value;
    }

    const auto found = std::upper_bound(
        trace.samples.begin(),
        trace.samples.end(),
        timestampNs,
        [](std::uint64_t at, const SignalSample& pt) { return at < pt.timestampNs; });

    if (found == trace.samples.begin()) {
        return trace.samples.front().value;
    }
    return (found - 1)->value;
}

// ---------------------------------------------------------------------------
// Geometry & Subplots
// ---------------------------------------------------------------------------

QRectF PlotView::totalPlotArea() const
{
    const double top = m_title.isEmpty() ? kTopBaseMargin : kTopTitleMargin;
    const double bottom =
        m_timeFormat == TimeDisplayFormat::AbsoluteDateTime ? kBottomDateMargin : kBottomBaseMargin;
    return QRectF{rect()}.adjusted(kLeftMargin, top, -kRightMargin, -bottom);
}

std::vector<PlotView::SubplotInfo> PlotView::computeSubplots() const
{
    const QRectF total = totalPlotArea();
    if (m_traces.empty()) {
        SubplotInfo single;
        single.index = 0;
        single.area = total;
        return {single};
    }

    std::vector<SubplotInfo> subplots;

    if (m_layoutMode == SubplotLayoutMode::SinglePlot) {
        SubplotInfo single;
        single.index = 0;
        single.area = total;
        double minVal = m_traces.front().minimum;
        double maxVal = m_traces.front().maximum;
        for (std::size_t i = 0; i < m_traces.size(); ++i) {
            single.traceIndices.push_back(i);
            minVal = std::min(minVal, m_traces[i].minimum);
            maxVal = std::max(maxVal, m_traces[i].maximum);
        }
        if (std::abs(maxVal - minVal) < 1e-9) {
            minVal -= 1.0;
            maxVal += 1.0;
        }
        single.minimum = minVal;
        single.maximum = maxVal;
        subplots.push_back(single);
        return subplots;
    }

    // AutoByUnit or Manual: Group traces
    struct Group {
        QString key;
        QString title;
        QString unit;
        std::vector<std::size_t> indices;
        double minVal{1e18};
        double maxVal{-1e18};
    };
    std::vector<Group> groups;

    for (std::size_t i = 0; i < m_traces.size(); ++i) {
        const PlotTrace& trace = m_traces[i];
        QString key = m_layoutMode == SubplotLayoutMode::Manual
                          ? QString::number(trace.subplotIndex)
                          : trace.unit.trimmed();

        auto it = std::find_if(
            groups.begin(), groups.end(), [&](const Group& g) { return g.key == key; });

        if (it == groups.end()) {
            Group newG;
            newG.key = key;
            newG.unit = trace.unit;
            newG.title = trace.name;
            if (!trace.unit.isEmpty()) {
                if (trace.unit.compare(QStringLiteral("V"), Qt::CaseInsensitive) == 0) {
                    newG.title = tr("Voltage");
                } else if (trace.unit.compare(QStringLiteral("l"), Qt::CaseInsensitive) == 0) {
                    newG.title = tr("Tank Level");
                } else if (trace.unit.compare(QStringLiteral("rpm"), Qt::CaseInsensitive) == 0) {
                    newG.title = tr("Engine Speed");
                }
            }
            newG.indices.push_back(i);
            newG.minVal = std::min(newG.minVal, trace.minimum);
            newG.maxVal = std::max(newG.maxVal, trace.maximum);
            groups.push_back(newG);
        } else {
            it->indices.push_back(i);
            it->minVal = std::min(it->minVal, trace.minimum);
            it->maxVal = std::max(it->maxVal, trace.maximum);
        }
    }

    // Limit to max 4 subplots
    const std::size_t count = std::min<std::size_t>(groups.size(), 4);
    const double totalHeight = total.height() - (count - 1) * kSubplotGap;
    const double subplotH = std::max(24.0, totalHeight / static_cast<double>(count));

    for (std::size_t i = 0; i < count; ++i) {
        SubplotInfo info;
        info.index = static_cast<int>(i);
        info.title = groups[i].title;
        info.unit = groups[i].unit;
        info.traceIndices = groups[i].indices;

        double minV = groups[i].minVal;
        double maxV = groups[i].maxVal;
        if (std::abs(maxV - minV) < 1e-9) {
            minV -= 1.0;
            maxV += 1.0;
        } else {
            // 5% headroom
            const double pad = (maxV - minV) * 0.05;
            minV -= pad;
            maxV += pad;
        }
        info.minimum = minV;
        info.maximum = maxV;

        const double topY = total.top() + static_cast<double>(i) * (subplotH + kSubplotGap);
        info.area = QRectF{total.left(), topY, total.width(), subplotH};
        subplots.push_back(info);
    }

    return subplots;
}

double PlotView::xFor(std::uint64_t timestampNs, const QRectF& area) const
{
    const double span = static_cast<double>(m_endNs - m_startNs);
    const double offset =
        static_cast<double>(timestampNs > m_startNs ? timestampNs - m_startNs : 0);
    return area.left() + std::clamp(offset / span, 0.0, 1.0) * area.width();
}

std::uint64_t PlotView::timestampForX(double x, const QRectF& area) const
{
    if (area.width() < 1.0) {
        return m_startNs;
    }
    const double fraction = std::clamp((x - area.left()) / area.width(), 0.0, 1.0);
    const double span = static_cast<double>(m_endNs - m_startNs);
    return m_startNs + static_cast<std::uint64_t>(fraction * span);
}

double PlotView::yFor(double value, double minimum, double maximum, const QRectF& area)
{
    if (!std::isfinite(value) || !std::isfinite(minimum) || !std::isfinite(maximum)) {
        return area.center().y();
    }
    if (maximum - minimum < 1e-12) {
        return area.center().y();
    }
    const double fraction = (value - minimum) / (maximum - minimum);
    return area.bottom() - std::clamp(fraction, 0.0, 1.0) * area.height();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void PlotView::paintEvent(QPaintEvent* /*event*/)
{
    const Theme theme = currentTheme();
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    // High-contrast dark automotive background
    painter.fillRect(rect(), theme.canvas);

    paintTitle(painter);

    if (m_traces.empty() && !m_placeholder.isEmpty()) {
        painter.setPen(theme.textMuted);
        painter.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }

    const std::vector<SubplotInfo> subplots = computeSubplots();
    if (subplots.empty()) {
        return;
    }

    const QRectF totalArea = totalPlotArea();

    if (m_showGrid) {
        paintGrid(painter, subplots);
    }

    // Paint each subplot's axis labels and trace data
    for (const SubplotInfo& subplot : subplots) {
        paintSubplotAxes(painter, subplot);

        for (std::size_t traceIdx : subplot.traceIndices) {
            if (traceIdx < m_traces.size()) {
                paintTrace(painter, subplot, m_traces[traceIdx]);
            }
        }
    }

    // Paint X-axis ticks and labels at the bottom of the lowest subplot
    paintTimeAxis(painter, subplots.back().area);

    // Cursors & measurements
    if (m_dualCursors) {
        paintDualCursors(painter, totalArea, subplots);
        paintMeasurementBanner(painter, totalArea);
    } else {
        paintSingleCursor(painter, totalArea);
    }

    // Live values legend overlay
    if (m_showLegend && !m_traces.empty()) {
        paintLegend(painter, totalArea);
    }

    // Rubber-band zoom box
    if (m_isZooming) {
        paintZoomBox(painter);
    }
}

void PlotView::paintTitle(QPainter& painter) const
{
    if (m_title.isEmpty()) {
        return;
    }
    const Theme theme = currentTheme();
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 2);
    painter.setFont(font);
    painter.setPen(theme.text);

    painter.drawText(QRectF{0, 4.0, static_cast<double>(width()), 22.0}, Qt::AlignCenter, m_title);
}

void PlotView::paintGrid(QPainter& painter, const std::vector<SubplotInfo>& subplots) const
{
    const Theme theme = currentTheme();
    const QRectF total = totalPlotArea();

    // Vertical time grid lines across all subplots
    painter.setPen(QPen{theme.canvasGridCoarse, 1.0, Qt::DotLine});
    for (int division = 0; division <= kTimeDivisions; ++division) {
        const double x = total.left() + total.width() * division / kTimeDivisions;
        painter.drawLine(QPointF{x, total.top()}, QPointF{x, total.bottom()});
    }

    // Horizontal grid lines per subplot
    for (const SubplotInfo& subplot : subplots) {
        for (int div = 0; div <= kValueDivisions; ++div) {
            const double y = subplot.area.top() + subplot.area.height() * div / kValueDivisions;
            painter.drawLine(QPointF{subplot.area.left(), y}, QPointF{subplot.area.right(), y});
        }
    }
}

void PlotView::paintSubplotAxes(QPainter& painter, const SubplotInfo& subplot) const
{
    const Theme theme = currentTheme();
    const QRectF& area = subplot.area;

    // Solid border line on left and bottom
    painter.setPen(QPen{theme.textMuted, 1.0});
    painter.drawLine(area.topLeft(), area.bottomLeft());
    painter.drawLine(area.bottomLeft(), area.bottomRight());

    const QFontMetrics metrics{painter.font()};

    // Value ticks and numbers down the left
    for (int div = 0; div <= kValueDivisions; ++div) {
        const double y = area.top() + area.height() * div / kValueDivisions;
        painter.drawLine(QPointF{area.left() - 4.0, y}, QPointF{area.left(), y});

        const double val =
            subplot.maximum - (subplot.maximum - subplot.minimum) * div / kValueDivisions;
        const QString label = formatValue(val);
        const double textW = metrics.horizontalAdvance(label);
        painter.drawText(QPointF{area.left() - textW - 6.0, y + metrics.ascent() / 2.0 - 1.0},
                         label);
    }

    // Vertical Subplot Label rotated 90 degrees along the left axis
    QString axisTitle = subplot.title;
    if (axisTitle.isEmpty()) {
        axisTitle = subplot.unit.isEmpty() ? QStringLiteral("Subplot %1").arg(subplot.index + 1)
                                           : QStringLiteral("[%1]").arg(subplot.unit);
    }

    painter.save();
    painter.translate(area.left() - 48.0, area.center().y());
    painter.rotate(-90.0);
    painter.setPen(theme.text);
    QFont titleFont = painter.font();
    titleFont.setBold(true);
    painter.setFont(titleFont);
    const double titleW = painter.fontMetrics().horizontalAdvance(axisTitle);
    painter.drawText(QPointF{-titleW / 2.0, 0.0}, axisTitle);
    painter.restore();
}

void PlotView::paintTimeAxis(QPainter& painter, const QRectF& bottomSubplotArea) const
{
    const Theme theme = currentTheme();
    const std::uint64_t span = m_endNs - m_startNs;
    const QFontMetrics metrics{painter.font()};

    painter.setPen(theme.textMuted);

    for (int division = 0; division <= kTimeDivisions; ++division) {
        const double x =
            bottomSubplotArea.left() + bottomSubplotArea.width() * division / kTimeDivisions;
        const std::uint64_t at = m_startNs
                                 + span * static_cast<std::uint64_t>(division)
                                       / static_cast<std::uint64_t>(kTimeDivisions);

        // Tick mark
        painter.drawLine(QPointF{x, bottomSubplotArea.bottom()},
                         QPointF{x, bottomSubplotArea.bottom() + 4.0});

        QString label;
        if (m_timeFormat == TimeDisplayFormat::AbsoluteDateTime) {
            label = formatDateTime(m_baseDateTime, at, span);
        } else {
            label = formatSeconds(at, span);
        }

        const double width = metrics.horizontalAdvance(label);
        double left = x - width / 2.0;
        left = std::clamp(left, 0.0, static_cast<double>(this->width()) - width);

        painter.drawText(QPointF{left, bottomSubplotArea.bottom() + metrics.ascent() + 4.0}, label);
    }

    // Centered axis label below the ticks
    const QString axisLabel =
        m_timeFormat == TimeDisplayFormat::AbsoluteDateTime ? tr("Date and Time") : tr("Time [s]");
    QFont labelFont = painter.font();
    labelFont.setBold(true);
    painter.setFont(labelFont);
    painter.setPen(theme.textMuted);

    const double labelW = painter.fontMetrics().horizontalAdvance(axisLabel);
    const double labelY = bottomSubplotArea.bottom() + metrics.ascent()
                          + (m_timeFormat == TimeDisplayFormat::AbsoluteDateTime ? 22.0 : 16.0);
    painter.drawText(QPointF{bottomSubplotArea.center().x() - labelW / 2.0, labelY}, axisLabel);
}

void PlotView::paintTrace(QPainter& painter,
                          const SubplotInfo& subplot,
                          const PlotTrace& trace) const
{
    if (trace.muted || trace.samples.empty()) {
        return;
    }

    const QRectF& area = subplot.area;
    const int maxColumns = std::max(1, static_cast<int>(area.width()));
    QPolygonF line;

    if (trace.samples.size() <= static_cast<std::size_t>(maxColumns)) {
        line.reserve(static_cast<qsizetype>(trace.samples.size()));
        for (const SignalSample& point : trace.samples) {
            line.append(QPointF{xFor(point.timestampNs, area),
                                yFor(point.value, subplot.minimum, subplot.maximum, area)});
        }
    } else {
        // High-density decimation preserving min/max envelope per column
        line.reserve(static_cast<qsizetype>(maxColumns * 2 + 2));

        int currentCol = -1;
        double colX = 0.0;
        double minY = 0.0;
        double maxY = 0.0;
        double lastY = 0.0;

        const auto flushColumn = [&]() {
            if (currentCol < 0) {
                return;
            }
            if (std::abs(maxY - minY) > 0.5) {
                line.append(QPointF{colX, minY});
                line.append(QPointF{colX, maxY});
            }
            line.append(QPointF{colX, lastY});
        };

        for (const SignalSample& point : trace.samples) {
            const double x = xFor(point.timestampNs, area);
            const double y = yFor(point.value, subplot.minimum, subplot.maximum, area);
            const int col = static_cast<int>(x - area.left());

            if (col != currentCol) {
                flushColumn();
                currentCol = col;
                colX = x;
                minY = y;
                maxY = y;
                lastY = y;
            } else {
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
                lastY = y;
                colX = x;
            }
        }
        flushColumn();
    }

    if (line.isEmpty()) {
        return;
    }

    // Hatched or Solid area fill under the curve (e.g. Tank Level in PCAN-Explorer)
    if (trace.fillStyle == PlotTraceFill::Hatched || trace.fillStyle == PlotTraceFill::Solid) {
        QPolygonF fillPoly = line;
        fillPoly.append(QPointF{line.back().x(), area.bottom()});
        fillPoly.append(QPointF{line.front().x(), area.bottom()});

        if (trace.fillStyle == PlotTraceFill::Hatched) {
            // Subtle translucent background tint
            QColor wash = trace.colour;
            wash.setAlpha(25);
            painter.setPen(Qt::NoPen);
            painter.setBrush(wash);
            painter.drawPolygon(fillPoly);

            // Diagonal hatching brush
            QColor hatchCol = trace.colour;
            hatchCol.setAlpha(120);
            painter.setBrush(QBrush{hatchCol, Qt::BDiagPattern});
            painter.drawPolygon(fillPoly);
        } else {
            QColor solidCol = trace.colour;
            solidCol.setAlpha(45);
            painter.setPen(Qt::NoPen);
            painter.setBrush(solidCol);
            painter.drawPolygon(fillPoly);
        }
    }

    // Draw main stroke
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{trace.colour, trace.lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
    painter.drawPolyline(line);

    // Current newest point marker
    painter.setPen(Qt::NoPen);
    painter.setBrush(trace.colour);
    painter.drawEllipse(line.back(), 2.8, 2.8);
}

void PlotView::paintSingleCursor(QPainter& painter, const QRectF& area) const
{
    if (m_cursorX < area.left() || m_cursorX > area.right()) {
        return;
    }

    const Theme theme = currentTheme();
    QColor line = theme.accent;
    line.setAlpha(160);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{line, 1.2, Qt::DashLine});
    painter.drawLine(QPointF{m_cursorX, area.top()}, QPointF{m_cursorX, area.bottom()});
}

void PlotView::paintDualCursors(QPainter& painter,
                                const QRectF& totalArea,
                                const std::vector<SubplotInfo>& /*subplots*/) const
{
    const double xA = xFor(m_cursorANs, totalArea);
    const double xB = xFor(m_cursorBNs, totalArea);

    // Translucent shading between Cursor A and Cursor B
    const double minX = std::min(xA, xB);
    const double maxX = std::max(xA, xB);
    if (maxX - minX > 1.0) {
        painter.fillRect(QRectF{minX, totalArea.top(), maxX - minX, totalArea.height()},
                         QColor{255, 255, 255, 14});
    }

    // Cursor A (Cyan #00D2FF)
    const QColor colA{0, 210, 255};
    painter.setPen(QPen{colA, 1.2, Qt::DashLine});
    painter.drawLine(QPointF{xA, totalArea.top()}, QPointF{xA, totalArea.bottom()});

    // Handle badge A
    painter.setPen(Qt::NoPen);
    painter.setBrush(colA);
    painter.drawRoundedRect(QRectF{xA - 9.0, totalArea.top() - 14.0, 18.0, 14.0}, 3.0, 3.0);
    painter.setPen(Qt::black);
    QFont f = painter.font();
    f.setBold(true);
    f.setPixelSize(10);
    painter.setFont(f);
    painter.drawText(QRectF{xA - 9.0, totalArea.top() - 14.0, 18.0, 14.0}, Qt::AlignCenter, "A");

    // Cursor B (Orange #FF9500)
    const QColor colB{255, 149, 0};
    painter.setPen(QPen{colB, 1.2, Qt::DashLine});
    painter.drawLine(QPointF{xB, totalArea.top()}, QPointF{xB, totalArea.bottom()});

    // Handle badge B
    painter.setPen(Qt::NoPen);
    painter.setBrush(colB);
    painter.drawRoundedRect(QRectF{xB - 9.0, totalArea.top() - 14.0, 18.0, 14.0}, 3.0, 3.0);
    painter.setPen(Qt::black);
    painter.drawText(QRectF{xB - 9.0, totalArea.top() - 14.0, 18.0, 14.0}, Qt::AlignCenter, "B");
}

void PlotView::paintMeasurementBanner(QPainter& painter, const QRectF& totalArea) const
{
    const Theme theme = currentTheme();

    const double tA = static_cast<double>(m_cursorANs) / 1'000'000'000.0;
    const double tB = static_cast<double>(m_cursorBNs) / 1'000'000'000.0;
    const qint64 deltaNs = static_cast<qint64>(m_cursorBNs) - static_cast<qint64>(m_cursorANs);
    const double deltaSec = static_cast<double>(deltaNs) / 1'000'000'000.0;
    const double freq = std::abs(deltaSec) > 1e-6 ? 1.0 / std::abs(deltaSec) : 0.0;

    QString banner = tr("A: %1 s   B: %2 s   |   Δt: %3 s (%4 Hz)")
                         .arg(tA, 0, 'f', 3)
                         .arg(tB, 0, 'f', 3)
                         .arg(deltaSec, 0, 'f', 3)
                         .arg(freq, 0, 'f', 2);

    // Delta Y for active traces
    int deltaCount = 0;
    for (const PlotTrace& trace : m_traces) {
        if (trace.muted || deltaCount >= 3) {
            continue;
        }
        const double yA = valueAtTimestamp(trace, m_cursorANs);
        const double yB = valueAtTimestamp(trace, m_cursorBNs);
        const double dY = yB - yA;
        banner += tr("   Δ%1: %2%3 %4")
                      .arg(trace.name)
                      .arg(dY >= 0.0 ? QStringLiteral("+") : QStringLiteral(""))
                      .arg(dY, 0, 'f', 2)
                      .arg(trace.unit);
        deltaCount++;
    }

    const QFontMetrics metrics{painter.font()};
    const double bannerW = metrics.horizontalAdvance(banner) + 24.0;
    const QRectF bannerRect{totalArea.left() + 10.0, totalArea.top() + 6.0, bannerW, 22.0};

    painter.setPen(QPen{theme.accent, 1.0});
    painter.setBrush(QColor{16, 20, 26, 225});
    painter.drawRoundedRect(bannerRect, 4.0, 4.0);

    painter.setPen(theme.text);
    painter.drawText(bannerRect, Qt::AlignCenter, banner);
}

QRectF PlotView::legendRect(const QRectF& totalArea) const
{
    const double rowH = 18.0;
    const double headerH = 20.0;
    const double h = headerH + static_cast<double>(m_traces.size()) * rowH + 8.0;
    const double w = 210.0;
    return QRectF{totalArea.right() - w - 8.0, totalArea.top() + 8.0, w, h};
}

int PlotView::hitTestLegendRow(const QPointF& pos, const QRectF& totalArea) const
{
    const QRectF rect = legendRect(totalArea);
    if (!rect.contains(pos)) {
        return -1;
    }
    const double relativeY = pos.y() - (rect.top() + 20.0);
    if (relativeY < 0.0) {
        return -1;
    }
    const int row = static_cast<int>(relativeY / 18.0);
    if (row >= 0 && row < static_cast<int>(m_traces.size())) {
        return row;
    }
    return -1;
}

void PlotView::paintLegend(QPainter& painter, const QRectF& totalArea) const
{
    const Theme theme = currentTheme();
    const QRectF rect = legendRect(totalArea);

    // Semi-transparent rounded overlay container
    painter.setPen(QPen{QColor{50, 60, 75, 180}, 1.0});
    painter.setBrush(QColor{14, 18, 22, 225});
    painter.drawRoundedRect(rect, 4.0, 4.0);

    // Header: Name | Y | Unit
    QFont headerFont = painter.font();
    headerFont.setBold(true);
    headerFont.setPixelSize(11);
    painter.setFont(headerFont);
    painter.setPen(theme.textMuted);

    const double y0 = rect.top() + 14.0;
    painter.drawText(QPointF{rect.left() + 26.0, y0}, tr("Name"));
    painter.drawText(QPointF{rect.left() + 120.0, y0}, tr("Y"));
    painter.drawText(QPointF{rect.left() + 168.0, y0}, tr("Unit"));

    painter.setPen(QPen{theme.canvasGridCoarse, 1.0});
    painter.drawLine(QPointF{rect.left() + 6.0, rect.top() + 20.0},
                     QPointF{rect.right() - 6.0, rect.top() + 20.0});

    // Rows
    QFont rowFont = painter.font();
    rowFont.setBold(false);
    rowFont.setPixelSize(11);
    painter.setFont(rowFont);

    // Active inspection timestamp: cursor timestamp if available, else latest
    std::uint64_t readoutTs = 0;
    if (m_dualCursors) {
        readoutTs = m_cursorBNs;
    } else if (m_cursorX >= totalArea.left() && m_cursorX <= totalArea.right()) {
        readoutTs = timestampForX(m_cursorX, totalArea);
    }

    double rowY = rect.top() + 34.0;
    for (const PlotTrace& trace : m_traces) {
        const QColor col = trace.muted ? theme.textMuted : trace.colour;

        // Color indicator pill
        painter.fillRect(QRectF{rect.left() + 8.0, rowY - 6.0, 12.0, 3.0}, col);

        // Name
        painter.setPen(trace.muted ? theme.textMuted : theme.text);
        const QString nameDisplay =
            painter.fontMetrics().elidedText(trace.name, Qt::ElideRight, 88);
        painter.drawText(QPointF{rect.left() + 26.0, rowY}, nameDisplay);

        // Value
        const double val = valueAtTimestamp(trace, readoutTs);
        const QString valStr = QString::number(val, 'f', 2);
        painter.drawText(QPointF{rect.left() + 120.0, rowY}, valStr);

        // Unit
        painter.setPen(theme.textMuted);
        painter.drawText(QPointF{rect.left() + 168.0, rowY}, trace.unit);

        rowY += 18.0;
    }
}

void PlotView::paintZoomBox(QPainter& painter) const
{
    const Theme theme = currentTheme();
    const QRectF box = QRectF{m_zoomBoxStart, m_zoomBoxCurrent}.normalized();

    QColor fill = theme.accent;
    fill.setAlpha(40);
    painter.fillRect(box, fill);

    painter.setPen(QPen{theme.accent, 1.0, Qt::DashLine});
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box);
}

// ---------------------------------------------------------------------------
// Mouse Interaction
// ---------------------------------------------------------------------------

void PlotView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPointF pos = event->position();
    const QRectF area = totalPlotArea();

    // 1. Legend hit test (toggle mute)
    if (m_showLegend && !m_traces.empty()) {
        const int hit = hitTestLegendRow(pos, area);
        if (hit >= 0 && hit < static_cast<int>(m_traces.size())) {
            m_traces[hit].muted = !m_traces[hit].muted;
            Q_EMIT traceToggled(hit, m_traces[hit].muted);
            update();
            return;
        }
    }

    // 2. Dual cursor handle drag
    if (m_dualCursors) {
        const double xA = xFor(m_cursorANs, area);
        const double xB = xFor(m_cursorBNs, area);

        if (std::abs(pos.x() - xA) <= 8.0) {
            m_draggedCursor = DraggedCursor::A;
            setCursor(Qt::SizeHorCursor);
            return;
        }
        if (std::abs(pos.x() - xB) <= 8.0) {
            m_draggedCursor = DraggedCursor::B;
            setCursor(Qt::SizeHorCursor);
            return;
        }
    }

    // 3. Navigation tool modes
    if (m_toolMode == PlotToolMode::Pan) {
        m_isPanning = true;
        m_panStartX = pos.x();
        m_panStartStartNs = m_startNs;
        m_panStartEndNs = m_endNs;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (m_toolMode == PlotToolMode::ZoomBox) {
        m_isZooming = true;
        m_zoomBoxStart = pos;
        m_zoomBoxCurrent = pos;
        update();
        return;
    }

    // 4. Pointer mode click sets primary cursor
    if (pos.x() >= area.left() && pos.x() <= area.right()) {
        m_cursorX = pos.x();
        const std::uint64_t ts = timestampForX(m_cursorX, area);
        Q_EMIT cursorMoved(ts);
        update();
    }
}

void PlotView::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    const QRectF area = totalPlotArea();

    // 1. Dragging dual cursor
    if (m_draggedCursor != DraggedCursor::None) {
        const std::uint64_t ts = timestampForX(pos.x(), area);
        if (m_draggedCursor == DraggedCursor::A) {
            m_cursorANs = ts;
        } else {
            m_cursorBNs = ts;
        }
        Q_EMIT measurementChanged(m_cursorANs,
                                  m_cursorBNs,
                                  static_cast<qint64>(m_cursorBNs)
                                      - static_cast<qint64>(m_cursorANs));
        update();
        return;
    }

    // 2. Panning time window
    if (m_isPanning && area.width() > 1.0) {
        const double dx = pos.x() - m_panStartX;
        const double span = static_cast<double>(m_panStartEndNs - m_panStartStartNs);
        const double shiftNs = -(dx / area.width()) * span;

        std::int64_t newStart =
            static_cast<std::int64_t>(m_panStartStartNs) + static_cast<std::int64_t>(shiftNs);
        std::int64_t newEnd =
            static_cast<std::int64_t>(m_panStartEndNs) + static_cast<std::int64_t>(shiftNs);

        if (newStart < 0) {
            newEnd += -newStart;
            newStart = 0;
        }

        m_startNs = static_cast<std::uint64_t>(newStart);
        m_endNs = static_cast<std::uint64_t>(newEnd);
        Q_EMIT timeRangeChanged(m_startNs, m_endNs);
        update();
        return;
    }

    // 3. Zoom box
    if (m_isZooming) {
        m_zoomBoxCurrent = pos;
        update();
        return;
    }

    // 4. Hover updates
    if (m_dualCursors) {
        const double xA = xFor(m_cursorANs, area);
        const double xB = xFor(m_cursorBNs, area);
        if (std::abs(pos.x() - xA) <= 6.0 || std::abs(pos.x() - xB) <= 6.0) {
            setCursor(Qt::SizeHorCursor);
        } else if (m_toolMode == PlotToolMode::Pan) {
            setCursor(Qt::OpenHandCursor);
        } else if (m_toolMode == PlotToolMode::ZoomBox) {
            setCursor(Qt::CrossCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }

    if (pos.x() < area.left() || pos.x() > area.right() || area.width() < 1.0) {
        if (m_cursorX >= 0.0) {
            m_cursorX = -1.0;
            update();
            Q_EMIT cursorLeft();
        }
        return;
    }

    m_cursorX = pos.x();
    update();

    const std::uint64_t ts = timestampForX(m_cursorX, area);
    Q_EMIT cursorMoved(ts);
}

void PlotView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_draggedCursor != DraggedCursor::None) {
        m_draggedCursor = DraggedCursor::None;
        setCursor(m_toolMode == PlotToolMode::Pan ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }

    if (m_isPanning) {
        m_isPanning = false;
        setCursor(Qt::OpenHandCursor);
    }

    if (m_isZooming) {
        m_isZooming = false;
        const QRectF area = totalPlotArea();
        const QRectF box = QRectF{m_zoomBoxStart, m_zoomBoxCurrent}.normalized();

        if (box.width() >= 10.0 && area.width() > 1.0) {
            const std::uint64_t t1 = timestampForX(box.left(), area);
            const std::uint64_t t2 = timestampForX(box.right(), area);
            setWindow(std::min(t1, t2), std::max(t1, t2));
            Q_EMIT timeRangeChanged(m_startNs, m_endNs);
        }
        update();
    }

    QWidget::mouseReleaseEvent(event);
}

void PlotView::mouseDoubleClickEvent(QMouseEvent* /*event*/)
{
    Q_EMIT autoScaleRequested();
}

void PlotView::wheelEvent(QWheelEvent* event)
{
    const QRectF area = totalPlotArea();
    if (area.width() < 1.0) {
        return;
    }

    const double mouseX = event->position().x();
    if (mouseX < area.left() || mouseX > area.right()) {
        return;
    }

    const double fraction = (mouseX - area.left()) / area.width();
    const double span = static_cast<double>(m_endNs - m_startNs);
    const double mouseTs = static_cast<double>(m_startNs) + fraction * span;

    // Zoom in / out factor
    const double factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
    const double newSpan = std::max(1'000'000.0, span * factor);

    const double newStart = std::max(0.0, mouseTs - fraction * newSpan);
    const double newEnd = newStart + newSpan;

    setWindow(static_cast<std::uint64_t>(newStart), static_cast<std::uint64_t>(newEnd));
    Q_EMIT timeRangeChanged(m_startNs, m_endNs);
    event->accept();
}

void PlotView::leaveEvent(QEvent* event)
{
    if (m_cursorX >= 0.0) {
        m_cursorX = -1.0;
        update();
        Q_EMIT cursorLeft();
    }
    QWidget::leaveEvent(event);
}

} // namespace torquebus::ui
