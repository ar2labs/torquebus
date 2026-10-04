// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Graph panel: which signals to draw, and the drawing of them.
//
// Supports PCAN-Explorer / automotive-grade features:
// - Interactive toolbar with Play/Pause/Stop, Navigation tools (Pan, Pointer, Zoom box)
// - Quick zoom in/out, fit to window / auto-scale
// - Dual measurement cursors (Cursor A & Cursor B with Δt / ΔY readouts)
// - Stacked subplots grouped by physical unit or single canvas
// - Fine dotted/dashed grid, hatched or solid curve area fills
// - Live values legend overlay table
// - Relative or absolute Date/Time axis formatting
// - X-Axis span selector dropdown
// - High-resolution image export (PNG / clipboard) and CSV export
// - Signal list filter search bar and per-signal color/fill styling

#pragma once

#include "core/plot/SignalSeries.h"
#include "ui/graph/PlotView.h"

#include <QAction>
#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include <cstdint>
#include <vector>

class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QTimer;
class QToolBar;

namespace torquebus::ui {

class GraphPanel final : public QWidget {
    Q_OBJECT

public:
    explicit GraphPanel(QWidget* parent = nullptr);

    /// Attaches the store this panel plots. Not owned; must outlive the panel.
    void setStore(SignalSeriesStore* store);

    /// Divider between signal list and plot canvas.
    [[nodiscard]] QByteArray splitterState() const;
    void restoreSplitterState(const QByteArray& state);

    [[nodiscard]] PlotView* plotView() const noexcept { return m_plot; }

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void refresh();

    void onPlay();
    void onPause();
    void onStop();
    void onClear();

    void onToolPointerToggled(bool checked);
    void onToolPanToggled(bool checked);
    void onToolZoomBoxToggled(bool checked);

    void onZoomIn();
    void onZoomOut();
    void onFitAll();

    void onDualCursorsToggled(bool checked);
    void onSubplotsToggled(bool checked);
    void onLegendToggled(bool checked);
    void onTimeFormatToggled(bool checked);

    void onExportPng();
    void onCopyImage();
    void onExportCsv();
    void onSettings();

    void onWindowChanged(int index);
    void onSelectionChanged(QListWidgetItem* item);
    void onSignalFilterChanged(const QString& text);
    void onSignalContextMenu(const QPoint& pos);

    void onCursorMoved(quint64 timestampNs);
    void onCursorLeft();
    void onTimeRangeChanged(quint64 startNs, quint64 endNs);
    void onMeasurementChanged(quint64 cursorANs, quint64 cursorBNs, qint64 deltaNs);
    void onThemeChanged();

private:
    struct SignalCustomStyle {
        QColor customColor;
        bool hasCustomColor{false};
        PlotTraceFill fillStyle{PlotTraceFill::None};
        int subplotIndex{0};
    };

    void buildUi();
    void setupToolbar();
    void syncSignalList();

    [[nodiscard]] std::vector<SeriesId> selectedSeries() const;
    [[nodiscard]] QColor colourFor(std::size_t index, SeriesId id) const;

    void updateStatus();
    void updatePlayPauseUi();
    void updateSignalItemVisuals(QListWidgetItem* item, SeriesId id);

    SignalSeriesStore* m_store{nullptr};

    QSplitter* m_splitter{nullptr};
    QWidget* m_leftSidebar{nullptr};
    QLineEdit* m_signalFilter{nullptr};
    QListWidget* m_signals{nullptr};
    PlotView* m_plot{nullptr};
    QToolBar* m_toolBar{nullptr};
    QComboBox* m_window{nullptr};
    QLabel* m_status{nullptr};
    QTimer* m_timer{nullptr};

    // Actions
    QAction* m_actionPlay{nullptr};
    QAction* m_actionPause{nullptr};
    QAction* m_actionStop{nullptr};
    QAction* m_actionClear{nullptr};

    QActionGroup* m_toolGroup{nullptr};
    QAction* m_actionPointer{nullptr};
    QAction* m_actionPan{nullptr};
    QAction* m_actionZoomBox{nullptr};

    QAction* m_actionZoomIn{nullptr};
    QAction* m_actionZoomOut{nullptr};
    QAction* m_actionFitAll{nullptr};

    QAction* m_actionDualCursors{nullptr};
    QAction* m_actionSubplots{nullptr};
    QAction* m_actionLegend{nullptr};
    QAction* m_actionTimeFormat{nullptr};

    QAction* m_actionExport{nullptr};
    QAction* m_actionSettings{nullptr};

    std::vector<SeriesId> m_selected;
    std::vector<SeriesWindow> m_windows;
    QHash<QString, SeriesId> m_listed;
    QHash<SeriesId, SignalCustomStyle> m_customStyles;

    bool m_frozen{false};
    bool m_userInteracting{false};
    bool m_refreshing{false};
    bool m_tracesDirty{true};

    std::uint64_t m_windowNs{10'000'000'000ULL};
    std::uint64_t m_lastNewestNs{0};
    std::uint64_t m_lastWindowNs{0};

    QString m_cursorText;
    PlotTraceFill m_defaultFill{PlotTraceFill::None};
    double m_defaultLineWidth{1.6};
};

} // namespace torquebus::ui
