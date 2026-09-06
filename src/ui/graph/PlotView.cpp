// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/graph/PlotView.h"

#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <utility>

namespace torquebus::ui {
namespace {

/// Room for the time labels along the bottom and the value labels down the
/// left. The left margin is generous because a value label carries a unit.
constexpr double kLeftMargin = 62.0;
constexpr double kRightMargin = 10.0;
constexpr double kTopMargin = 8.0;
constexpr double kBottomMargin = 20.0;

/// Vertical grid lines. Enough to read a time off, few enough not to compete
/// with the data - the same judgement the canvas grid makes.
constexpr int kTimeDivisions = 6;
constexpr int kValueDivisions = 4;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// Seconds, with enough decimals to tell two grid lines apart.
[[nodiscard]] QString formatSeconds(std::uint64_t nanoseconds, std::uint64_t spanNs)
{
    const double seconds = static_cast<double>(nanoseconds) / 1'000'000'000.0;

    // A ten-second window wants tenths; a hundred-millisecond one wants
    // thousandths. Fixed decimals would print "1.0, 1.0, 1.0" on the second.
    const int decimals = spanNs < 2'000'000'000ULL ? 3 : (spanNs < 60'000'000'000ULL ? 2 : 1);

    return QStringLiteral("%1").arg(seconds, 0, 'f', decimals);
}

/// A value, short enough for an axis label.
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
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);
}

QSize PlotView::sizeHint() const
{
    return {640, 320};
}

QSize PlotView::minimumSizeHint() const
{
    return {240, 120};
}

void PlotView::setTraces(std::vector<PlotTrace> traces)
{
    m_traces = std::move(traces);
    update();
}

void PlotView::setWindow(std::uint64_t startNs, std::uint64_t endNs)
{
    // A window with no width would divide by zero below and, more to the point,
    // is not a window. One millisecond is the floor.
    m_startNs = startNs;
    m_endNs = std::max(endNs, startNs + 1'000'000ULL);

    update();
}

void PlotView::setPlaceholder(const QString& text)
{
    if (text == m_placeholder) {
        return;
    }

    m_placeholder = text;
    update();
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

QRectF PlotView::plotArea() const
{
    return QRectF{rect()}.adjusted(kLeftMargin, kTopMargin, -kRightMargin, -kBottomMargin);
}

double PlotView::xFor(std::uint64_t timestampNs, const QRectF& area) const
{
    const double span = static_cast<double>(m_endNs - m_startNs);

    // Clamped rather than allowed off the edge: a sample slightly outside the
    // window still anchors the line that leaves it, and a polyline running to
    // x = -40000 is a repaint the rasteriser does not enjoy.
    const double offset =
        static_cast<double>(timestampNs > m_startNs ? timestampNs - m_startNs : 0);

    return area.left() + std::clamp(offset / span, 0.0, 1.0) * area.width();
}

double PlotView::yFor(double value, double minimum, double maximum, const QRectF& area)
{
    // A flat signal has no range to scale against. Centring it is the honest
    // answer: it says "this did not change", where stretching a zero range to
    // the full height would turn rounding noise into a mountain.
    if (maximum - minimum < 1e-12) {
        return area.center().y();
    }

    const double fraction = (value - minimum) / (maximum - minimum);

    // Inverted, because a plot grows upwards and a widget's y grows down.
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

    // The canvas colour, not the panel's: a plot is a surface data floats above,
    // the same relationship the pipeline canvas has with its blocks.
    painter.fillRect(rect(), theme.canvas);

    const QRectF area = plotArea();
    if (area.width() < 2.0 || area.height() < 2.0) {
        return;
    }

    paintGrid(painter, area);

    if (m_traces.empty() && !m_placeholder.isEmpty()) {
        painter.setPen(theme.textMuted);
        painter.drawText(rect(), Qt::AlignCenter, m_placeholder);
        return;
    }

    for (std::size_t index = 0; index < m_traces.size(); ++index) {
        paintTrace(painter, area, m_traces[index]);
        paintScale(painter, area, m_traces[index], static_cast<int>(index));
    }

    paintCursor(painter, area);
}

void PlotView::paintGrid(QPainter& painter, const QRectF& area) const
{
    const Theme theme = currentTheme();
    const std::uint64_t span = m_endNs - m_startNs;

    painter.setPen(QPen{theme.canvasGridCoarse, 1.0});

    for (int division = 0; division <= kTimeDivisions; ++division) {
        const double x = area.left() + area.width() * division / kTimeDivisions;
        painter.drawLine(QPointF{x, area.top()}, QPointF{x, area.bottom()});
    }

    for (int division = 0; division <= kValueDivisions; ++division) {
        const double y = area.top() + area.height() * division / kValueDivisions;
        painter.drawLine(QPointF{area.left(), y}, QPointF{area.right(), y});
    }

    // Time labels along the bottom, in seconds from the start of the
    // measurement - the same epoch the trace's timestamps use, so a row in one
    // and a point in the other are the same instant.
    painter.setPen(theme.textMuted);

    const QFontMetrics metrics{painter.font()};

    for (int division = 0; division <= kTimeDivisions; ++division) {
        const double x = area.left() + area.width() * division / kTimeDivisions;
        const std::uint64_t at = m_startNs + span * static_cast<std::uint64_t>(division)
                                                 / static_cast<std::uint64_t>(kTimeDivisions);

        const QString label = formatSeconds(at, span);
        const double width = metrics.horizontalAdvance(label);

        // The first and last labels are pulled inside the plot rather than
        // hanging off it, so neither is clipped by the widget's edge.
        double left = x - width / 2.0;
        left = std::clamp(left, 0.0, static_cast<double>(this->width()) - width);

        painter.drawText(QPointF{left, area.bottom() + metrics.ascent() + 4.0}, label);
    }
}

void PlotView::paintTrace(QPainter& painter, const QRectF& area, const PlotTrace& trace) const
{
    if (trace.samples.empty()) {
        return;
    }

    QPolygonF line;
    line.reserve(static_cast<qsizetype>(trace.samples.size()));

    for (const SignalSample& point : trace.samples) {
        line.append(QPointF{xFor(point.timestampNs, area),
                            yFor(point.value, trace.minimum, trace.maximum, area)});
    }

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{trace.colour, 1.6, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin});
    painter.drawPolyline(line);

    // The newest point, marked. On a signal that updates slowly the line's end
    // is the only thing that says where "now" is.
    painter.setPen(Qt::NoPen);
    painter.setBrush(trace.colour);
    painter.drawEllipse(line.back(), 2.5, 2.5);
}

void PlotView::paintScale(QPainter& painter,
                          const QRectF& area,
                          const PlotTrace& trace,
                          int row) const
{
    if (trace.samples.empty()) {
        return;
    }

    const QFontMetrics metrics{painter.font()};
    const double lineHeight = metrics.height();

    // Four traces' worth of labels fill the margin. Past that they would start
    // overlapping, and a wrong number is worse than no number - so they stop.
    if ((row + 1) * lineHeight > area.height() / 2.0) {
        return;
    }

    painter.setPen(trace.colour);

    const auto label = [&](const QString& text, double y) {
        const double width = metrics.horizontalAdvance(text);
        painter.drawText(QPointF{area.left() - width - 5.0, y}, text);
    };

    // A flat trace is drawn down the middle, so labelling a top and a bottom it
    // does not have would be inventing a scale. One value, where the line is.
    if (trace.maximum - trace.minimum < 1e-12) {
        label(formatValue(trace.maximum), area.center().y() + metrics.ascent() / 2.0);
        return;
    }

    label(formatValue(trace.maximum), area.top() + metrics.ascent() + row * lineHeight);
    label(formatValue(trace.minimum), area.bottom() - row * lineHeight);
}

void PlotView::paintCursor(QPainter& painter, const QRectF& area) const
{
    if (m_cursorX < area.left() || m_cursorX > area.right()) {
        return;
    }

    const Theme theme = currentTheme();

    QColor line = theme.accent;
    line.setAlpha(140);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{line, 1.0, Qt::DashLine});
    painter.drawLine(QPointF{m_cursorX, area.top()}, QPointF{m_cursorX, area.bottom()});
}

// ---------------------------------------------------------------------------
// The cursor
// ---------------------------------------------------------------------------

void PlotView::mouseMoveEvent(QMouseEvent* event)
{
    const QRectF area = plotArea();
    const double x = event->position().x();

    if (x < area.left() || x > area.right() || area.width() < 1.0) {
        if (m_cursorX >= 0.0) {
            m_cursorX = -1.0;
            update();
            Q_EMIT cursorLeft();
        }
        return;
    }

    m_cursorX = x;
    update();

    const double fraction = (x - area.left()) / area.width();
    const auto span = static_cast<double>(m_endNs - m_startNs);

    Q_EMIT cursorMoved(static_cast<quint64>(m_startNs + static_cast<std::uint64_t>(fraction * span)));
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
