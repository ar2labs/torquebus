// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The line the bus drew.
//
// Painted by hand rather than with a charting library, for the same reason the
// dock chrome is: the plot has to sit inside this theme, follow the accent the
// user chose, and share the canvas's idea of what a grid looks like. A chart
// widget brings its own opinion about all three and a dependency to argue with
// about each one.
//
// It draws what it is handed and owns no history. The panel reads a window out
// of the SignalSeriesStore on its own timer and passes it in - so this widget
// never touches the store, never blocks the executor, and can be pointed at a
// recorded window later without knowing the difference.
//
// **Every series gets its own vertical scale.** An engine speed in the
// thousands and a coolant temperature in the tens on one axis is a straight
// line and a flat line; on their own scales it is two signals you can read.
// That is the choice every tool in this family makes, and the reason the axis
// labels are drawn per series rather than once down the left.

#pragma once

#include "core/plot/SignalSeries.h"

#include <QColor>
#include <QList>
#include <QtGlobal>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace torquebus::ui {

/// One series, ready to draw: the samples, the scale, and the colour.
struct PlotTrace final {
    QString name;
    QString unit;
    QColor colour;

    /// Oldest first, as SignalSeriesStore hands them over.
    std::vector<SignalSample> samples;

    double minimum{};
    double maximum{};
};

class PlotView final : public QWidget {
    Q_OBJECT

public:
    explicit PlotView(QWidget* parent = nullptr);

    /// Replaces what is drawn. Cheap enough to call at the panel's refresh
    /// rate: the traces are moved in, not copied.
    void setTraces(std::vector<PlotTrace> traces);

    /// The time span the horizontal axis covers, in nanoseconds.
    void setWindow(std::uint64_t startNs, std::uint64_t endNs);

    [[nodiscard]] std::uint64_t windowStartNs() const noexcept { return m_startNs; }
    [[nodiscard]] std::uint64_t windowEndNs() const noexcept { return m_endNs; }

    /// Shown in the middle when there is nothing to draw, in place of an empty
    /// grid that looks like a plot which has stopped working.
    void setPlaceholder(const QString& text);

Q_SIGNALS:
    /// The user moved the cursor over the plot. The panel turns this into a
    /// readout; the view has no idea what a legend is.
    void cursorMoved(quint64 timestampNs);

    /// The cursor left the plot area.
    void cursorLeft();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

private:
    /// The rectangle the lines are drawn in, inside the axis margins.
    [[nodiscard]] QRectF plotArea() const;

    /// Where `timestampNs` falls horizontally, in widget coordinates.
    [[nodiscard]] double xFor(std::uint64_t timestampNs, const QRectF& area) const;

    /// Where `value` falls vertically for a trace with that range.
    [[nodiscard]] static double yFor(double value,
                                     double minimum,
                                     double maximum,
                                     const QRectF& area);

    void paintGrid(QPainter& painter, const QRectF& area) const;
    void paintTrace(QPainter& painter, const QRectF& area, const PlotTrace& trace) const;

    /// The range of one trace, at the left edge, in that trace's colour.
    ///
    /// Per trace and not once down the side, because each has its own vertical
    /// scale - a single axis would be labelling a scale that only one of the
    /// lines is actually drawn against.
    void paintScale(QPainter& painter,
                    const QRectF& area,
                    const PlotTrace& trace,
                    int row) const;
    void paintCursor(QPainter& painter, const QRectF& area) const;

    std::vector<PlotTrace> m_traces;

    std::uint64_t m_startNs{0};
    std::uint64_t m_endNs{1'000'000'000};

    QString m_placeholder;

    /// Where the mouse is, in widget coordinates, or -1 when it is elsewhere.
    double m_cursorX{-1.0};
};

} // namespace torquebus::ui
