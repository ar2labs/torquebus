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
// Supports automotive-grade analysis features inspired by tools like PCAN-Explorer:
// multi-subplot stacking grouped by physical unit, fine dotted/dashed grids,
// hatched/solid curve area fills, interactive legend with live value readouts,
// dual measurement cursors (Cursor A & Cursor B with delta readout), pan/zoom
// box tools, and relative or absolute date/time formatting.

#pragma once

#include "core/plot/SignalSeries.h"

#include <QColor>
#include <QDateTime>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include <cstdint>
#include <vector>

class QPainter;
class QPaintEvent;
class QMouseEvent;
class QWheelEvent;
class QEvent;

namespace torquebus::ui {

/// Visual fill style under a curve.
enum class PlotTraceFill : std::uint8_t {
    None = 0,
    Hatched, // Diagonal hatching (e.g. Tank Level in PCAN-Explorer)
    Solid // Semi-transparent solid fill
};

/// Navigation / interaction tool mode for the plot canvas.
enum class PlotToolMode : std::uint8_t {
    Pointer = 0, // Normal cursor scrubbing & point inspection
    Pan, // Drag time window left/right
    ZoomBox, // Drag rubber-band box to zoom into a region
    DualCursor // Dual measurement cursors (Cursor A & Cursor B)
};

/// How subplots are arranged vertically.
enum class SubplotLayoutMode : std::uint8_t {
    AutoByUnit = 0, // Group signals by physical unit (e.g. V on top, l on bottom)
    SinglePlot, // All signals plotted in one shared canvas
    Manual // Assigned per trace via subplotIndex
};

/// Format of the horizontal time axis.
enum class TimeDisplayFormat : std::uint8_t {
    RelativeSeconds = 0, // 0.000 s
    AbsoluteDateTime // dd.MM.yyyy hh:mm:ss
};

/// One series, ready to draw: the samples, the scale, and the colour.
struct PlotTrace final {
    QString name;
    QString unit;
    QColor colour;

    /// Oldest first, as SignalSeriesStore hands them over.
    std::vector<SignalSample> samples;

    double minimum{};
    double maximum{};

    int subplotIndex{0};
    PlotTraceFill fillStyle{PlotTraceFill::None};
    double lineWidth{1.6};
    bool muted{false};
};

class PlotView final : public QWidget {
    Q_OBJECT

public:
    explicit PlotView(QWidget* parent = nullptr);

    /// Replaces what is drawn. Cheap enough to call at the panel's refresh
    /// rate: the traces are moved in, not copied.
    void setTraces(std::vector<PlotTrace> traces);
    [[nodiscard]] const std::vector<PlotTrace>& traces() const noexcept { return m_traces; }
    [[nodiscard]] std::vector<PlotTrace>& traces() noexcept { return m_traces; }

    /// The time span the horizontal axis covers, in nanoseconds.
    void setWindow(std::uint64_t startNs, std::uint64_t endNs);

    [[nodiscard]] std::uint64_t windowStartNs() const noexcept { return m_startNs; }
    [[nodiscard]] std::uint64_t windowEndNs() const noexcept { return m_endNs; }

    /// Centered plot title displayed at the top (e.g. "MicroMod FD Pots").
    void setTitle(const QString& title);
    [[nodiscard]] const QString& title() const noexcept { return m_title; }

    /// Shown in the middle when there is nothing to draw.
    void setPlaceholder(const QString& text);
    [[nodiscard]] const QString& placeholder() const noexcept { return m_placeholder; }

    /// Current interactive navigation tool.
    void setToolMode(PlotToolMode mode);
    [[nodiscard]] PlotToolMode toolMode() const noexcept { return m_toolMode; }

    /// Subplot layout mode.
    void setSubplotLayoutMode(SubplotLayoutMode mode);
    [[nodiscard]] SubplotLayoutMode subplotLayoutMode() const noexcept { return m_layoutMode; }

    /// Time axis formatting (relative elapsed seconds vs absolute date/time).
    void setTimeDisplayFormat(TimeDisplayFormat format);
    [[nodiscard]] TimeDisplayFormat timeDisplayFormat() const noexcept { return m_timeFormat; }

    /// Reference date/time for absolute timestamp formatting.
    void setBaseDateTime(const QDateTime& dt);
    [[nodiscard]] const QDateTime& baseDateTime() const noexcept { return m_baseDateTime; }

    /// Legend overlay visibility.
    void setShowLegend(bool show);
    [[nodiscard]] bool showLegend() const noexcept { return m_showLegend; }

    /// Grid lines visibility.
    void setShowGrid(bool show);
    [[nodiscard]] bool showGrid() const noexcept { return m_showGrid; }

    /// Dual measurement cursors.
    void setDualCursorsEnabled(bool enabled);
    [[nodiscard]] bool dualCursorsEnabled() const noexcept { return m_dualCursors; }

    void setCursorAPosition(std::uint64_t timestampNs);
    void setCursorBPosition(std::uint64_t timestampNs);
    [[nodiscard]] std::uint64_t cursorAPosition() const noexcept { return m_cursorANs; }
    [[nodiscard]] std::uint64_t cursorBPosition() const noexcept { return m_cursorBNs; }

    /// Number of subplots currently generated.
    [[nodiscard]] int subplotCount() const;

    /// Evaluates sample value for a trace at a given timestamp.
    [[nodiscard]] double valueAtTimestamp(const PlotTrace& trace, std::uint64_t timestampNs) const;

Q_SIGNALS:
    /// The user moved the cursor over the plot.
    void cursorMoved(quint64 timestampNs);

    /// The cursor left the plot area.
    void cursorLeft();

    /// Emitted when user drags or zooms the time window.
    void timeRangeChanged(quint64 startNs, quint64 endNs);

    /// Emitted when user double-clicks or asks to auto-scale.
    void autoScaleRequested();

    /// Emitted when dual measurement cursors are moved.
    void measurementChanged(quint64 cursorANs, quint64 cursorBNs, qint64 deltaNs);

    /// Emitted when a trace is muted or unmuted via legend interaction.
    void traceToggled(int traceIndex, bool muted);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

private:
    struct SubplotInfo {
        int index{0};
        QString title;
        QString unit;
        double minimum{0.0};
        double maximum{1.0};
        QRectF area;
        std::vector<std::size_t> traceIndices;
    };

    /// Total plot canvas area inside the outer axis margins.
    [[nodiscard]] QRectF totalPlotArea() const;

    /// Computes the vertical subplots layout and bounds.
    [[nodiscard]] std::vector<SubplotInfo> computeSubplots() const;

    /// Where `timestampNs` falls horizontally, in widget coordinates.
    [[nodiscard]] double xFor(std::uint64_t timestampNs, const QRectF& area) const;
    [[nodiscard]] std::uint64_t timestampForX(double x, const QRectF& area) const;

    /// Where `value` falls vertically for a trace/subplot with that range.
    [[nodiscard]] static double
    yFor(double value, double minimum, double maximum, const QRectF& area);

    void paintTitle(QPainter& painter) const;
    void paintGrid(QPainter& painter, const std::vector<SubplotInfo>& subplots) const;
    void paintSubplotAxes(QPainter& painter, const SubplotInfo& subplot) const;
    void paintTimeAxis(QPainter& painter, const QRectF& bottomSubplotArea) const;
    void paintTrace(QPainter& painter, const SubplotInfo& subplot, const PlotTrace& trace) const;
    void paintSingleCursor(QPainter& painter, const QRectF& area) const;
    void paintDualCursors(QPainter& painter,
                          const QRectF& totalArea,
                          const std::vector<SubplotInfo>& subplots) const;
    void paintLegend(QPainter& painter, const QRectF& totalArea) const;
    void paintZoomBox(QPainter& painter) const;
    void paintMeasurementBanner(QPainter& painter, const QRectF& totalArea) const;

    [[nodiscard]] QRectF legendRect(const QRectF& totalArea) const;
    [[nodiscard]] int hitTestLegendRow(const QPointF& pos, const QRectF& totalArea) const;

    std::vector<PlotTrace> m_traces;

    std::uint64_t m_startNs{0};
    std::uint64_t m_endNs{1'000'000'000};

    QString m_title;
    QString m_placeholder;

    PlotToolMode m_toolMode{PlotToolMode::Pointer};
    SubplotLayoutMode m_layoutMode{SubplotLayoutMode::AutoByUnit};
    TimeDisplayFormat m_timeFormat{TimeDisplayFormat::RelativeSeconds};
    QDateTime m_baseDateTime{QDateTime::currentDateTime()};

    bool m_showLegend{true};
    bool m_showGrid{true};
    bool m_dualCursors{false};

    /// Dual measurement cursor positions in nanoseconds
    std::uint64_t m_cursorANs{200'000'000};
    std::uint64_t m_cursorBNs{800'000'000};

    /// Mouse interaction tracking
    double m_cursorX{-1.0};
    bool m_isPanning{false};
    double m_panStartX{0.0};
    std::uint64_t m_panStartStartNs{0};
    std::uint64_t m_panStartEndNs{0};

    bool m_isZooming{false};
    QPointF m_zoomBoxStart;
    QPointF m_zoomBoxCurrent;

    enum class DraggedCursor : std::uint8_t { None, A, B };
    DraggedCursor m_draggedCursor{DraggedCursor::None};
};

} // namespace torquebus::ui
