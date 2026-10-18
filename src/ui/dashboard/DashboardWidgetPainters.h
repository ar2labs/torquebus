// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How the newer Dashboard widgets draw themselves.
//
// The panel paints by hand (DashboardPanel.h says why), and the first eight kinds are methods of
// it. The kinds that came later are free functions here, for two reasons that go together: a
// function that is given a painter, a rectangle and a reading can be handed a QImage in a test and
// its pixels looked at, which a method of a panel that needs a plot store, a clock and a window
// cannot; and the geometry that decides where things are - the track of a bar, the chip of a state
// - is a function of the rectangle alone, so the panel, the painter and the test all ask the same
// one.
//
// Colour comes from the theme and from nowhere else. A tone is a meaning (DashboardTone), and what
// "danger" looks like is the theme's to say, so a dashboard built on the dark theme reads the same
// on the light one.

#pragma once

#include "core/dashboard/DashboardDescription.h"

#include <QColor>
#include <QRectF>
#include <QString>

#include <cstdint>
#include <vector>

class QPainter;

namespace torquebus {
struct SeriesWindow;
} // namespace torquebus

namespace torquebus::ui {

struct Theme;
class DashboardImages;

/// What a widget is showing right now, and whether it is showing anything.
struct DashboardReading final {
    double value{0.0};
    bool known{false};

    /// For a CAN signal, the timestamp of the sample this is: which one it is, not when. The
    /// cluster and the stale check watch it for change, to tell a signal that stopped from one that
    /// is steady.
    std::uint64_t stamp{0};
    bool timed{false};

    /// Not known because the signal has been silent for longer than the widget allows, and not
    /// because it never spoke. The two are drawn alike - dashes - and told apart by a badge.
    bool stale{false};
};

/// A row of a Table: a signal, and what it reads.
struct TableRow final {
    QString name;
    QString value;
    DashboardTone tone{DashboardTone::Neutral};
    bool known{false};
};

/// Everything a painter is given about the widget it draws.
struct PaintContext final {
    const Theme& theme;
    const DashboardWidget& widget;
    QRectF rect;
    DashboardReading reading;

    /// The widget's title, or what it is bound to: the text under it.
    QString caption;

    DashboardImages& images;

    /// Edit mode: a widget with nothing to show says what it is waiting for.
    bool editing{false};
};

// --- Shared ----------------------------------------------------------------------------------

/// What a tone is called by the theme.
[[nodiscard]] QColor toneColor(const Theme& theme, DashboardTone tone);

/// `colour` at `alpha` (0 to 255), for the wash behind a tinted thing.
[[nodiscard]] QColor withAlpha(const QColor& colour, int alpha);

/// Where `value` sits between the bounds, 0 to 1.
[[nodiscard]] double fractionOf(double value, double minimum, double maximum);

/// What a reading says in words: the name of its state if the widget has one, otherwise the number
/// with its unit, and dashes when it is not known.
[[nodiscard]] QString formatReading(const DashboardWidget& widget, const DashboardReading& reading);

// --- Geometry ---------------------------------------------------------------------------------

/// The groove a Bar fills, along its longer side.
[[nodiscard]] QRectF barTrackRect(const QRectF& rect);

/// The chip a State draws its name on.
[[nodiscard]] QRectF stateChipRect(const QRectF& rect);

/// Where a Trend's line is drawn.
[[nodiscard]] QRectF trendPlotRect(const QRectF& rect);

inline constexpr double kTableTitleHeight = 30.0;
inline constexpr double kTableRowHeight = 22.0;

/// How many rows of a Table fit in `rect`, with room left for a "more" line when they do not all.
[[nodiscard]] int tableRowCapacity(const QRectF& rect, int rowCount);

// --- Painters ---------------------------------------------------------------------------------

void paintBar(QPainter& painter, const PaintContext& context);
void paintState(QPainter& painter, const PaintContext& context);
void paintImage(QPainter& painter, const PaintContext& context);

/// `window` is the signal's recent samples, or null when it has none; `newestNs` is the newest
/// timestamp in the store, which the right edge of the plot is.
void paintTrend(QPainter& painter,
                const PaintContext& context,
                const SeriesWindow* window,
                std::uint64_t newestNs);

/// `emptyHint` is what to say when there are no rows to show.
void paintTable(QPainter& painter,
                const PaintContext& context,
                const std::vector<TableRow>& rows,
                const QString& emptyHint);

void paintGroup(QPainter& painter, const PaintContext& context);

/// The small mark a widget wears when its signal has gone quiet.
void paintStaleBadge(QPainter& painter, const PaintContext& context);

} // namespace torquebus::ui
