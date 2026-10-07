// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The dashboard: PLAN.md v0.14, the half that people look at.
//
// One panel, two modes, and the mode is the whole design:
//
//   * **Run** - what a bench looks like. Gauges move, lamps light, and a slider
//     under the hand writes a variable a simulated ECU is reading. Nothing can
//     be moved or deleted by accident, because the same click that operates a
//     control would otherwise drag it.
//
//   * **Edit** - what a designer looks like. Widgets are dragged, resized by a
//     corner, added from a menu and deleted. Controls do not respond, for the
//     same reason in reverse: dragging a slider into place must not send the
//     value it sweeps through on the way.
//
// A mode switch rather than a modifier key. A modifier is invisible, and a
// dashboard is used by people who did not build it - the state has to be on
// screen.
//
// Everything is painted by hand rather than assembled from QWidgets. A gauge is
// an arc, a needle and a scale; there is no QGauge, and the alternative is a
// QDial wearing a style sheet that fights it. Painting also makes the widgets
// resolution-independent for free, which is what lets a dashboard built on a
// laptop fill a bench monitor rather than sit in its corner.
//
// Where the values come from:
//
//   * a **signal** binding reads the plot store, which is where decoded signals
//     already accumulate for the Graph panel. No second decode path, and a
//     dashboard shows exactly what the Graph shows.
//   * a **variable** binding reads and writes SystemVariables, lock-free.
//
// Neither is polled per frame: the panel repaints at 20 Hz like every other,
// because a needle that updates faster than a screen refreshes is work nobody
// can see.

#pragma once

#include "core/dashboard/DashboardDescription.h"

#include <QPoint>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;

namespace torquebus {
class SignalSeriesStore;
class SystemVariables;
} // namespace torquebus

namespace torquebus::ui {

class ClusterHost;

class DashboardPanel final : public QWidget {
    Q_OBJECT

public:
    /// The description is not owned and must outlive the panel. Edits are
    /// written straight into it, like the canvas writes the graph.
    explicit DashboardPanel(DashboardDescription& dashboard, QWidget* parent = nullptr);
    ~DashboardPanel() override;

    /// Where signal bindings read from. Not owned; null shows every signal
    /// widget as having no value, which is what a stopped measurement is.
    void setPlotStore(const SignalSeriesStore* store);

    /// Where variable bindings read and write. Not owned.
    void setVariables(SystemVariables* variables);

    /// Rebuilds from the description - after a project is opened, or the
    /// dashboard is changed from outside.
    void reload();

    [[nodiscard]] bool isEditing() const noexcept { return m_editing; }

public Q_SLOTS:
    void setEditing(bool editing);

Q_SIGNALS:
    /// The description changed: a widget moved, resized, added or removed. The
    /// window marks the project dirty, exactly as for a canvas edit.
    void dashboardEdited();

    /// A line for the Output panel.
    void reported(const QString& text, bool isError);

    /// Which widget is selected in edit mode, or empty. The window shows its
    /// properties.
    void selectionChanged(const QString& widgetId);

protected:
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private Q_SLOTS:
    /// Repaints if anything a widget is bound to has moved. The panel's clock.
    void refresh();

private:
    /// What a widget is showing right now, and whether it is showing anything.
    struct Reading final {
        double value{0.0};
        bool known{false};

        /// For a CAN signal, the timestamp of the sample this is: which one it is, not when. The
        /// cluster watches it for change, to tell a signal that stopped from one that is steady.
        std::uint64_t stamp{0};
        bool timed{false};
    };

    [[nodiscard]] Reading read(const DashboardWidget& widget) const;

    /// The same, for a binding on its own: what a cluster asks for each of its roles.
    [[nodiscard]] Reading readBinding(const DashboardBinding& binding) const;

    /// Lays the QML clusters over the panel as the description and the mode say. See ClusterHost.
    void syncClusters();

    /// Writes a control's value where its binding points. Does nothing for a
    /// binding that cannot be written, which validate() has already refused -
    /// this is the second line of defence, not the first.
    void write(const DashboardWidget& widget, double value);

    /// The widget under a point, or nullptr. Topmost first: later widgets are
    /// drawn over earlier ones, so they are hit first too.
    [[nodiscard]] DashboardWidget* widgetAt(const QPoint& point);

    /// Where a widget is on screen, in pixels.
    [[nodiscard]] QRectF rectOf(const DashboardWidget& widget) const;

    /// The resize handle of a widget, in pixels.
    [[nodiscard]] QRectF handleOf(const DashboardWidget& widget) const;

    /// Turns a click inside a control into the value it means.
    [[nodiscard]] static double
    valueFromPoint(const DashboardWidget& widget, const QRectF& rect, const QPoint& point);

    void paintWidget(QPainter& painter, const DashboardWidget& widget);
    void paintGauge(QPainter& painter,
                    const DashboardWidget& widget,
                    const QRectF& rect,
                    const Reading& reading);
    void paintNumeric(QPainter& painter,
                      const DashboardWidget& widget,
                      const QRectF& rect,
                      const Reading& reading);
    void paintLamp(QPainter& painter,
                   const DashboardWidget& widget,
                   const QRectF& rect,
                   const Reading& reading);
    void paintSlider(QPainter& painter,
                     const DashboardWidget& widget,
                     const QRectF& rect,
                     const Reading& reading);
    void paintKnob(QPainter& painter,
                   const DashboardWidget& widget,
                   const QRectF& rect,
                   const Reading& reading);
    void paintButton(QPainter& painter,
                     const DashboardWidget& widget,
                     const QRectF& rect,
                     const Reading& reading);
    void paintLabel(QPainter& painter, const DashboardWidget& widget, const QRectF& rect);

    /// A cluster draws itself, in QML, over the panel; what the panel paints is what is under it
    /// when it is not there - Edit mode, or a cluster that could not load.
    void paintCluster(QPainter& painter, const DashboardWidget& widget, const QRectF& rect);

    /// The text under a widget: its title, or what it is bound to.
    [[nodiscard]] QString captionOf(const DashboardWidget& widget) const;

    /// Adds a widget of `kind` at `where`, selects it and reports the edit.
    void addWidget(DashboardWidgetKind kind, const QPoint& where);

    void removeSelected();

    DashboardDescription& m_dashboard;

    /// A member, not a child: it has to go before the QQuickWidgets it made, which are children.
    /// ClusterHost explains why.
    std::unique_ptr<ClusterHost> m_clusters;

    const SignalSeriesStore* m_plots{nullptr};
    SystemVariables* m_variables{nullptr};

    bool m_editing{false};

    /// The selected widget's id in edit mode, empty otherwise.
    std::string m_selected;

    /// What the mouse is doing to it.
    enum class Drag : std::uint8_t { None, Move, Resize, Operate };

    Drag m_drag{Drag::None};
    QPoint m_dragFrom;
    QRectF m_dragOrigin;

    /// The widget a Button is pressing, so the release can let it go even if
    /// the mouse has wandered off the button by then - which is what a real
    /// momentary contact does.
    std::string m_pressed;

    /// The revisions the last repaint was drawn from, so a panel with nothing
    /// moving does not repaint twenty times a second.
    std::vector<std::uint64_t> m_lastRevisions;
};

} // namespace torquebus::ui
