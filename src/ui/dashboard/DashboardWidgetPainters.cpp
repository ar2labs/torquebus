// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/DashboardWidgetPainters.h"

#include "core/plot/SignalSeries.h"
#include "ui/dashboard/DashboardImages.h"
#include "ui/theme/Theme.h"

#include <QFont>
#include <QFontMetricsF>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <optional>

namespace torquebus::ui {
namespace {

/// Inside every widget: the room between its edge and what it draws.
constexpr double kPad = 8.0;

[[nodiscard]] QFont sized(const QFont& base, double points, bool bold)
{
    QFont font = base;
    font.setPointSizeF(points);
    font.setBold(bold);
    return font;
}

/// `text` shortened with an ellipsis to fit `width` in the painter's current font.
[[nodiscard]] QString elided(const QPainter& painter, const QString& text, double width)
{
    return painter.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(width));
}

/// The muted line under or over a widget: its title.
void drawCaption(QPainter& painter, const PaintContext& c, const QRectF& band, Qt::Alignment align)
{
    painter.setFont(sized(painter.font(), std::clamp(c.rect.height() * 0.14, 8.0, 12.0), false));
    painter.setPen(c.theme.textMuted);
    painter.drawText(band, align | Qt::AlignVCenter, elided(painter, c.caption, band.width()));
}

/// The band at the foot of a widget that a caption sits in.
[[nodiscard]] QRectF footBand(const QRectF& rect)
{
    const double height = std::max(16.0, rect.height() * 0.26);
    return QRectF{rect.left() + 4.0, rect.bottom() - height, rect.width() - 8.0, height};
}

/// A dashed outline with a few words inside it, for a widget that has nothing to show yet.
void drawPlaceholder(QPainter& painter,
                     const PaintContext& c,
                     const QString& text,
                     const QColor& ink)
{
    QPen outline{c.theme.border, 1.0, Qt::DashLine};
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(c.rect.adjusted(1.0, 1.0, -1.0, -1.0), 6.0, 6.0);

    painter.setFont(sized(painter.font(), std::clamp(c.rect.height() * 0.12, 8.0, 11.0), false));
    painter.setPen(ink);
    painter.drawText(
        c.rect.adjusted(6.0, 6.0, -6.0, -6.0), Qt::AlignCenter | Qt::TextWordWrap, text);
}

} // namespace

// ---------------------------------------------------------------------------
// Shared
// ---------------------------------------------------------------------------

QColor toneColor(const Theme& theme, DashboardTone tone)
{
    switch (tone) {
    case DashboardTone::Ok:
        return theme.success;
    case DashboardTone::Info:
        return theme.accent;
    case DashboardTone::Warning:
        return theme.lampAmber;
    case DashboardTone::Danger:
        return theme.lampRed;
    case DashboardTone::Off:
        return theme.textMuted;
    case DashboardTone::Neutral:
        break;
    }

    return theme.text;
}

QColor withAlpha(const QColor& colour, int alpha)
{
    QColor result = colour;
    result.setAlpha(std::clamp(alpha, 0, 255));
    return result;
}

double fractionOf(double value, double minimum, double maximum)
{
    if (!(maximum > minimum) || std::isnan(value)) {
        return 0.0;
    }

    return std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
}

QString formatReading(const DashboardWidget& widget, const DashboardReading& reading)
{
    if (!reading.known) {
        return QStringLiteral("--");
    }

    if (const DashboardState* state = widget.stateOf(reading.value);
        state != nullptr && !state->label.empty()) {
        return QString::fromStdString(state->label);
    }

    QString text = QString::number(reading.value, 'f', widget.decimals);

    if (!widget.unit.empty()) {
        text += QLatin1Char(' ') + QString::fromStdString(widget.unit);
    }

    return text;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

QRectF barTrackRect(const QRectF& rect)
{
    if (rect.width() >= rect.height()) {
        const double thickness = std::clamp(rect.height() * 0.26, 8.0, 24.0);

        return QRectF{rect.left() + kPad,
                      rect.top() + rect.height() * 0.50,
                      std::max(1.0, rect.width() - 2.0 * kPad),
                      thickness};
    }

    const double thickness = std::clamp(rect.width() * 0.30, 10.0, 28.0);

    return QRectF{rect.center().x() - thickness / 2.0,
                  rect.top() + rect.height() * 0.22,
                  thickness,
                  std::max(1.0, rect.height() * 0.54)};
}

QRectF stateChipRect(const QRectF& rect)
{
    return rect.adjusted(4.0, 4.0, -4.0, -std::max(16.0, rect.height() * 0.28));
}

QRectF trendPlotRect(const QRectF& rect)
{
    QRectF plot = rect.adjusted(kPad, 24.0, -kPad, -16.0);

    // A box too short for a title and a scale still gets a plot.
    if (plot.height() < 12.0) {
        plot = rect.adjusted(4.0, 4.0, -4.0, -4.0);
    }

    return plot;
}

int tableRowCapacity(const QRectF& rect, int rowCount)
{
    const double room = rect.height() - kTableTitleHeight - 4.0;
    const int fits = std::max(0, static_cast<int>(std::floor(room / kTableRowHeight)));

    if (fits >= rowCount) {
        return rowCount;
    }

    // Not all of them: the last row's place goes to the line that says how many are not shown.
    return std::max(0, fits - 1);
}

// ---------------------------------------------------------------------------
// Bar
// ---------------------------------------------------------------------------

void paintBar(QPainter& painter, const PaintContext& c)
{
    const Theme& theme = c.theme;
    const DashboardWidget& widget = c.widget;
    const QRectF rect = c.rect;

    const bool horizontal = rect.width() >= rect.height();
    const bool known = c.reading.known;

    const double filled = known ? fractionOf(c.reading.value, widget.minimum, widget.maximum) : 0.0;
    const DashboardTone tone =
        known ? widget.limits.toneOf(c.reading.value) : DashboardTone::Neutral;
    const QColor fill = tone == DashboardTone::Neutral ? theme.accent : toneColor(theme, tone);

    painter.setRenderHint(QPainter::Antialiasing, true);

    // --- The groove and what fills it ------------------------------------------------------------
    const QRectF track = barTrackRect(rect);
    const double radius = (horizontal ? track.height() : track.width()) / 2.0;

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.border);
    painter.drawRoundedRect(track, radius, radius);

    if (known && filled > 0.0) {
        QRectF part = track;

        if (horizontal) {
            part.setWidth(std::max(track.height(), track.width() * filled));
        } else {
            part.setTop(track.bottom() - std::max(track.width(), track.height() * filled));
        }

        painter.setBrush(fill);
        painter.drawRoundedRect(part, radius, radius);
    }

    // --- Where the alarm limits are ------------------------------------------------------------
    const auto mark = [&](const std::optional<double>& limit, DashboardTone meaning) {
        if (!limit.has_value() || !(*limit > widget.minimum) || !(*limit < widget.maximum)) {
            return;
        }

        const double at = fractionOf(*limit, widget.minimum, widget.maximum);

        painter.setPen(QPen{toneColor(theme, meaning), 2.0, Qt::SolidLine, Qt::FlatCap});

        if (horizontal) {
            const double x = track.left() + track.width() * at;
            painter.drawLine(QPointF{x, track.top() - 2.0}, QPointF{x, track.bottom() + 2.0});
        } else {
            const double y = track.bottom() - track.height() * at;
            painter.drawLine(QPointF{track.left() - 2.0, y}, QPointF{track.right() + 2.0, y});
        }
    };

    mark(widget.limits.warningBelow, DashboardTone::Warning);
    mark(widget.limits.warningAbove, DashboardTone::Warning);
    mark(widget.limits.criticalBelow, DashboardTone::Danger);
    mark(widget.limits.criticalAbove, DashboardTone::Danger);

    // --- The words -----------------------------------------------------------------------------
    const QString value = formatReading(widget, c.reading);
    const QColor ink =
        known ? (tone == DashboardTone::Neutral ? theme.text : fill) : theme.textMuted;

    if (horizontal) {
        const QRectF top{rect.left() + kPad,
                         rect.top() + 2.0,
                         rect.width() - 2.0 * kPad,
                         track.top() - rect.top() - 4.0};

        painter.setFont(sized(painter.font(), std::clamp(rect.height() * 0.24, 9.0, 18.0), true));
        painter.setPen(ink);
        painter.drawText(top, Qt::AlignRight | Qt::AlignVCenter, value);

        const double valueWidth = QFontMetricsF{painter.font()}.horizontalAdvance(value);
        drawCaption(
            painter,
            c,
            QRectF{
                top.left(), top.top(), std::max(0.0, top.width() - valueWidth - 8.0), top.height()},
            Qt::AlignLeft);

        // The ends of the scale, when there is room under the groove for them.
        if (rect.height() >= 52.0) {
            painter.setFont(sized(painter.font(), 7.5, false));
            painter.setPen(theme.textMuted);

            const QRectF below{track.left(),
                               track.bottom() + 2.0,
                               track.width(),
                               rect.bottom() - track.bottom() - 3.0};

            painter.drawText(
                below, Qt::AlignLeft | Qt::AlignTop, QString::number(widget.minimum, 'g', 5));
            painter.drawText(
                below, Qt::AlignRight | Qt::AlignTop, QString::number(widget.maximum, 'g', 5));
        }
    } else {
        const QRectF top{rect.left() + 2.0,
                         rect.top() + 2.0,
                         rect.width() - 4.0,
                         track.top() - rect.top() - 4.0};

        painter.setFont(sized(painter.font(), std::clamp(rect.width() * 0.16, 8.0, 14.0), true));
        painter.setPen(ink);
        painter.drawText(top, Qt::AlignCenter, elided(painter, value, top.width()));

        drawCaption(painter, c, footBand(rect), Qt::AlignHCenter);
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void paintState(QPainter& painter, const PaintContext& c)
{
    const Theme& theme = c.theme;
    const DashboardWidget& widget = c.widget;

    const QRectF chip = stateChipRect(c.rect);
    const bool known = c.reading.known;

    const DashboardState* state = known ? widget.stateOf(c.reading.value) : nullptr;
    const DashboardTone tone = state != nullptr ? state->tone : DashboardTone::Neutral;
    const bool tinted = known && tone != DashboardTone::Neutral;

    const QColor line = !known ? theme.border : (tinted ? toneColor(theme, tone) : theme.border);
    const QColor wash = tinted ? withAlpha(toneColor(theme, tone), 46) : theme.panel;
    const QColor ink = !known ? theme.textMuted : (tinted ? toneColor(theme, tone) : theme.text);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen{line, tinted ? 2.0 : 1.0});
    painter.setBrush(known ? QBrush{wash} : QBrush{Qt::NoBrush});
    painter.drawRoundedRect(chip, 8.0, 8.0);

    painter.setFont(sized(painter.font(), std::clamp(chip.height() * 0.40, 9.0, 22.0), true));
    painter.setPen(ink);
    painter.drawText(chip.adjusted(6.0, 0.0, -6.0, 0.0),
                     Qt::AlignCenter,
                     elided(painter, formatReading(widget, c.reading), chip.width() - 12.0));

    drawCaption(painter, c, footBand(c.rect), Qt::AlignHCenter);
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

void paintImage(QPainter& painter, const PaintContext& c)
{
    const DashboardWidget& widget = c.widget;
    const bool bound = widget.binding.source != DashboardBinding::Source::None;

    // The picture of the state the value is in, or the widget's own for a value no state pictures.
    std::string path = widget.image;

    if (bound && c.reading.known) {
        if (const DashboardState* state = widget.stateOf(c.reading.value);
            state != nullptr && !state->image.empty()) {
            path = state->image;
        }
    }

    // A value that is not known is in no state, and "drop a PNG here" is not what a widget with a
    // picture for each state should say: it shows the first of them, turned down, which is what it
    // would be.
    if (path.empty() && bound && !c.reading.known) {
        for (const DashboardState& state : widget.states) {
            if (!state.image.empty()) {
                path = state.image;
                break;
            }
        }
    }

    // A caption only when somebody asked for one: a logo is not captioned by its file name.
    const bool captioned = !widget.title.empty();
    QRectF target = c.rect.adjusted(2.0, 2.0, -2.0, -2.0);

    if (captioned) {
        target.setBottom(footBand(c.rect).top());
    }

    if (path.empty()) {
        if (c.editing) {
            drawPlaceholder(painter, c, QObject::tr("Image\nDrop a PNG here"), c.theme.textMuted);
        }

        return;
    }

    // A bound picture is dimmed while the value is not known: it shows what it would be, not what
    // it is.
    const qreal opacity = bound && !c.reading.known ? 0.35 : 1.0;

    if (!c.images.draw(painter, target, path, opacity)) {
        // Both modes say so, quietly in run mode: a missing picture is a thing to fix, but a bench
        // is not the place to shout about it.
        drawPlaceholder(painter,
                        c,
                        c.editing
                            ? QObject::tr("Missing picture\n%1").arg(QString::fromStdString(path))
                            : QObject::tr("(no picture)"),
                        c.editing ? c.theme.lampAmber : c.theme.textMuted);
    }

    if (captioned) {
        drawCaption(painter, c, footBand(c.rect), Qt::AlignHCenter);
    }
}

// ---------------------------------------------------------------------------
// Trend
// ---------------------------------------------------------------------------

void paintTrend(QPainter& painter,
                const PaintContext& c,
                const SeriesWindow* window,
                std::uint64_t newestNs)
{
    const Theme& theme = c.theme;
    const DashboardWidget& widget = c.widget;

    painter.setRenderHint(QPainter::Antialiasing, true);

    // The box.
    painter.setPen(theme.border);
    painter.setBrush(theme.panel);
    painter.drawRoundedRect(c.rect, 4.0, 4.0);

    const QRectF plot = trendPlotRect(c.rect);

    // --- The scale -----------------------------------------------------------------------------
    painter.setPen(QPen{withAlpha(theme.border, 140), 1.0});
    for (int line = 0; line <= 4; ++line) {
        const double y = plot.top() + plot.height() * static_cast<double>(line) / 4.0;
        painter.drawLine(QPointF{plot.left(), y}, QPointF{plot.right(), y});
    }

    const auto yOf = [&](double value) {
        const double at = fractionOf(value, widget.minimum, widget.maximum);
        return plot.bottom() - at * plot.height();
    };

    // Limits, as dashed lines at the heights they are.
    const auto limitLine = [&](const std::optional<double>& limit, DashboardTone meaning) {
        if (!limit.has_value() || !(*limit > widget.minimum) || !(*limit < widget.maximum)) {
            return;
        }

        painter.setPen(QPen{withAlpha(toneColor(theme, meaning), 190), 1.2, Qt::DashLine});
        painter.drawLine(QPointF{plot.left(), yOf(*limit)}, QPointF{plot.right(), yOf(*limit)});
    };

    limitLine(widget.limits.warningBelow, DashboardTone::Warning);
    limitLine(widget.limits.warningAbove, DashboardTone::Warning);
    limitLine(widget.limits.criticalBelow, DashboardTone::Danger);
    limitLine(widget.limits.criticalAbove, DashboardTone::Danger);

    // --- The line ------------------------------------------------------------------------------
    const double windowNs = widget.windowSeconds * 1e9;
    const double startNs = static_cast<double>(newestNs) - windowNs;

    bool drew = false;
    DashboardTone lastTone = DashboardTone::Neutral;

    if (window != nullptr && !window->samples.empty() && windowNs > 0.0) {
        QPainterPath path;
        bool open = false;

        for (const SignalSample& sample : window->samples) {
            // "Not available" is a sample that is not a number: a gap, not a plunge to zero.
            if (!std::isfinite(sample.value)) {
                open = false;
                continue;
            }

            const double x =
                plot.left()
                + (static_cast<double>(sample.timestampNs) - startNs) / windowNs * plot.width();
            const double y = std::clamp(yOf(sample.value), plot.top() - 2.0, plot.bottom() + 2.0);

            if (!open) {
                path.moveTo(x, y);
                open = true;
            } else {
                path.lineTo(x, y);
            }

            drew = true;
            lastTone = widget.limits.toneOf(sample.value);
        }

        if (drew) {
            const QColor ink =
                lastTone == DashboardTone::Neutral ? theme.accent : toneColor(theme, lastTone);

            painter.save();
            painter.setClipRect(plot.adjusted(-1.0, -3.0, 1.0, 3.0));
            painter.setPen(QPen{ink, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            painter.restore();
        }
    }

    // --- The words -----------------------------------------------------------------------------
    painter.setFont(sized(painter.font(), std::clamp(c.rect.height() * 0.11, 8.0, 11.0), false));

    if (!drew) {
        painter.setPen(theme.textMuted);
        painter.drawText(plot, Qt::AlignCenter, QObject::tr("no data"));
    }

    const QRectF header{
        c.rect.left() + kPad, c.rect.top() + 3.0, c.rect.width() - 2.0 * kPad, 18.0};

    painter.setPen(theme.textMuted);
    painter.drawText(
        header, Qt::AlignLeft | Qt::AlignVCenter, elided(painter, c.caption, header.width() * 0.6));

    painter.setPen(c.reading.known ? theme.text : theme.textMuted);
    painter.drawText(header, Qt::AlignRight | Qt::AlignVCenter, formatReading(widget, c.reading));

    if (plot.height() >= 50.0) {
        painter.setFont(sized(painter.font(), 7.0, false));
        painter.setPen(theme.textMuted);
        painter.drawText(plot.adjusted(2.0, 1.0, 0.0, 0.0),
                         Qt::AlignLeft | Qt::AlignTop,
                         QString::number(widget.maximum, 'g', 5));
        painter.drawText(plot.adjusted(2.0, 0.0, 0.0, -1.0),
                         Qt::AlignLeft | Qt::AlignBottom,
                         QString::number(widget.minimum, 'g', 5));
    }
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

void paintTable(QPainter& painter,
                const PaintContext& c,
                const std::vector<TableRow>& rows,
                const QString& emptyHint)
{
    const Theme& theme = c.theme;
    const QRectF rect = c.rect;

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(theme.border);
    painter.setBrush(theme.panel);
    painter.drawRoundedRect(rect, 4.0, 4.0);

    // --- The title -----------------------------------------------------------------------------
    const QRectF title{
        rect.left() + kPad, rect.top(), rect.width() - 2.0 * kPad, kTableTitleHeight};

    painter.setFont(sized(painter.font(), std::clamp(kTableTitleHeight * 0.38, 9.0, 12.0), true));
    painter.setPen(theme.text);
    painter.drawText(
        title, Qt::AlignLeft | Qt::AlignVCenter, elided(painter, c.caption, title.width()));

    painter.setPen(withAlpha(theme.border, 200));
    painter.drawLine(QPointF{rect.left(), rect.top() + kTableTitleHeight},
                     QPointF{rect.right(), rect.top() + kTableTitleHeight});

    if (rows.empty()) {
        painter.setFont(sized(painter.font(), 9.0, false));
        painter.setPen(theme.textMuted);
        painter.drawText(rect.adjusted(kPad, kTableTitleHeight + 4.0, -kPad, -4.0),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         emptyHint);
        return;
    }

    // --- The rows ------------------------------------------------------------------------------
    const int shown = tableRowCapacity(rect, static_cast<int>(rows.size()));
    painter.setFont(sized(painter.font(), 9.0, false));

    for (int i = 0; i < shown; ++i) {
        const TableRow& row = rows[static_cast<std::size_t>(i)];
        const double top =
            rect.top() + kTableTitleHeight + 2.0 + kTableRowHeight * static_cast<double>(i);

        if (i % 2 == 1) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(theme.panelAlternate, 160));
            painter.drawRect(QRectF{rect.left() + 1.0, top, rect.width() - 2.0, kTableRowHeight});
        }

        const QRectF line{rect.left() + kPad, top, rect.width() - 2.0 * kPad, kTableRowHeight};

        const QString value = row.value;
        const double valueWidth =
            std::min(line.width() * 0.55, painter.fontMetrics().horizontalAdvance(value) + 4.0);

        painter.setPen(theme.text);
        painter.drawText(
            QRectF{line.left(), line.top(), line.width() - valueWidth - 6.0, line.height()},
            Qt::AlignLeft | Qt::AlignVCenter,
            elided(painter, row.name, line.width() - valueWidth - 6.0));

        painter.setPen(row.known ? (row.tone == DashboardTone::Neutral ? theme.text
                                                                       : toneColor(theme, row.tone))
                                 : theme.textMuted);
        painter.drawText(QRectF{line.right() - valueWidth, line.top(), valueWidth, line.height()},
                         Qt::AlignRight | Qt::AlignVCenter,
                         elided(painter, value, valueWidth));
    }

    if (shown < static_cast<int>(rows.size())) {
        const double top =
            rect.top() + kTableTitleHeight + 2.0 + kTableRowHeight * static_cast<double>(shown);

        painter.setPen(theme.textMuted);
        painter.drawText(
            QRectF{rect.left() + kPad, top, rect.width() - 2.0 * kPad, kTableRowHeight},
            Qt::AlignLeft | Qt::AlignVCenter,
            QObject::tr("+ %n more", nullptr, static_cast<int>(rows.size()) - shown));
    }
}

// ---------------------------------------------------------------------------
// Group, and the stale badge
// ---------------------------------------------------------------------------

void paintGroup(QPainter& painter, const PaintContext& c)
{
    const Theme& theme = c.theme;

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen{theme.border, 1.5});
    painter.setBrush(withAlpha(theme.panel, 110));
    painter.drawRoundedRect(c.rect.adjusted(1.0, 1.0, -1.0, -1.0), 10.0, 10.0);

    if (c.widget.title.empty()) {
        return;
    }

    painter.setFont(sized(painter.font(), 9.0, true));
    painter.setPen(theme.textMuted);

    const QRectF title{c.rect.left() + 12.0, c.rect.top() + 4.0, c.rect.width() - 24.0, 20.0};
    painter.drawText(title,
                     Qt::AlignLeft | Qt::AlignVCenter,
                     elided(painter, QString::fromStdString(c.widget.title), title.width()));
}

void paintStaleBadge(QPainter& painter, const PaintContext& c)
{
    painter.save();
    painter.setFont(sized(painter.font(), 7.5, true));
    painter.setPen(c.theme.lampAmber);
    painter.drawText(QRectF{c.rect.right() - 52.0, c.rect.top() + 2.0, 50.0, 12.0},
                     Qt::AlignRight | Qt::AlignVCenter,
                     QObject::tr("stale"));
    painter.restore();
}

} // namespace torquebus::ui
