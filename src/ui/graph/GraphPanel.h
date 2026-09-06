// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Graph panel: which signals to draw, and the drawing of them.
//
// The list on the left is the whole of the interaction. A measurement decodes
// whatever the database describes - forty signals is ordinary - and a plot that
// drew all of them would be a solid block of colour. So nothing is drawn until
// it is ticked, and the list is the record of what somebody wanted to watch.
//
// It reads the engine's SignalSeriesStore on a timer and never blocks the
// executor: the store hands over a copy of the window under its own lock, and
// this panel owns the buffer that copy lands in (see SignalSeries.h). At the
// refresh rate below that is one lock acquisition every 50 ms against a
// pipeline that may be running at a hundred thousand frames a second.

#pragma once

#include "core/plot/SignalSeries.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

class QAction;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QTimer;
class QToolBar;

namespace torquebus::ui {

class PlotView;

class GraphPanel final : public QWidget {
    Q_OBJECT

public:
    explicit GraphPanel(QWidget* parent = nullptr);

    /// Attaches the store this panel plots. Not owned; must outlive the panel.
    ///
    /// Not a const pointer, because Clear is a thing the user asks for and the
    /// panel is the only place that asks. Everything else it does is a read.
    void setStore(SignalSeriesStore* store);

    /// The divider between the signal list and the plot, for the settings file.
    [[nodiscard]] QByteArray splitterState() const;
    void restoreSplitterState(const QByteArray& state);

private Q_SLOTS:
    /// Re-reads the store and repaints. The panel's whole clock.
    void refresh();

    void onFreezeToggled(bool frozen);
    void onClear();
    void onWindowChanged(int index);
    void onSelectionChanged(QListWidgetItem* item);
    void onCursorMoved(quint64 timestampNs);
    void onCursorLeft();
    void onThemeChanged();

private:
    void buildUi();

    /// Adds a row for any signal the store has that the list does not.
    ///
    /// Signals appear as the bus introduces them, so the list grows during a
    /// measurement. Rebuilding it would lose the ticks somebody had just set,
    /// which is why this adds rather than replaces.
    void syncSignalList();

    /// The series the user has ticked, in list order.
    [[nodiscard]] std::vector<SeriesId> selectedSeries() const;

    /// Colour for the n-th selected series.
    ///
    /// Taken from the accent palette, which is already derived per theme and
    /// already checked for contrast against the surface it lands on - so a plot
    /// line is legible on both themes for the same reason a focus ring is.
    [[nodiscard]] QColor colourFor(std::size_t index) const;

    void updateStatus();

    SignalSeriesStore* m_store{nullptr};

    QSplitter* m_splitter{nullptr};
    QListWidget* m_signals{nullptr};
    PlotView* m_plot{nullptr};
    QToolBar* m_toolBar{nullptr};
    QComboBox* m_window{nullptr};
    QLabel* m_status{nullptr};
    QTimer* m_timer{nullptr};

    QAction* m_actionFreeze{nullptr};
    QAction* m_actionClear{nullptr};

    /// Reused every refresh, so a repaint allocates nothing once the series
    /// have settled - the same reason TraceModel keeps its own buffers.
    std::vector<SeriesId> m_selected;
    std::vector<SeriesWindow> m_windows;

    /// Series already in the list, by name, so syncSignalList knows what is new.
    QHash<QString, SeriesId> m_listed;

    bool m_frozen{false};

    /// How much time the plot shows, in nanoseconds.
    std::uint64_t m_windowNs{10'000'000'000ULL};

    /// The last value read under the cursor, per selected series, for the
    /// status line. Kept so leaving the plot can put the summary back.
    QString m_cursorText;
};

} // namespace torquebus::ui
