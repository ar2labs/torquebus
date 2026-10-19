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
//     already accumulate for the Plot Viewer. No second decode path, and a
//     dashboard shows exactly what the Plot Viewer shows.
//   * a **variable** binding reads and writes SystemVariables, lock-free.
//   * a **Faults** widget has no binding. It lists the trouble codes the J1939 block says the
//     bus is reporting - a snapshot the engine's thread publishes and this one copies when its
//     revision moves (J1939Network.h), never the block's own tables - and lights the four lamps
//     from the system variables the instrument cluster reads, so the two cannot disagree.
//
// None is polled per frame: the panel repaints at 20 Hz like every other,
// because a needle that updates faster than a screen refreshes is work nobody
// can see.
//
// --- What the databases add ----------------------------------------------------------------
//
// The panel is given the loaded databases (setDatabases) and builds a SignalDictionary from them,
// and the dictionary is what makes a dashboard easy to build rather than only possible:
//
//   * **Drag a signal in.** The DBC Explorer's signals and messages (SignalMime.h) drop onto the
//     panel in Edit mode and become the widget each wants - a lamp for a two-state signal, a bar
//     for a percentage, a table for a message - already bound, with its unit, range, names and
//     time-out filled in. Dropped on a widget that is already there, one signal re-binds it.
//   * **Drag a picture in.** A PNG, JPEG or SVG file dropped on the panel becomes an Image; dropped
//     on a widget that has a picture of its own, it becomes that widget's picture.
//   * **Say what is wrong.** In Edit mode a widget whose message or signal is in no loaded database
//     wears a mark, and hovering over it names the nearest thing that is. Nothing is refused: the
//     dashboard opens as often with the database unloaded as with it loaded.
//   * **Notice a silent signal.** A widget with a time-out stops showing a value that has not been
//     refreshed within it, and says so.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/DashboardFaults.h"
#include "core/dashboard/SignalDictionary.h"
#include "core/plot/SignalSeries.h"
#include "ui/dashboard/DashboardImages.h"
#include "ui/dashboard/DashboardWidgetPainters.h"

#include <QPoint>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QContextMenuEvent;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QKeyEvent;
class QMimeData;
class QMouseEvent;
class QPaintEvent;
class QPainter;

namespace torquebus {
class J1939NameTables;
class J1939Network;
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

    /// Where a Faults widget reads the trouble codes of the bus: the hand-over the engine's thread
    /// publishes to and a panel reads from (J1939Network.h). Not owned; null, or a bus that has
    /// published nothing, shows every Faults widget as an empty list, which is what a measurement
    /// with no J1939 block is.
    ///
    /// The panel only ever asks it for its revision - an atomic load, every tick - and, when that
    /// has moved, for a copy of what it holds. Nothing the executor thread is working on is reached
    /// any other way, and a pointer to const is how that is kept true.
    void setJ1939Network(const J1939Network* network);

    /// The words behind the numbers of an ECU's NAME, so that a row can say "Engine" and not only
    /// an address. Null is addresses only. Not owned.
    void setJ1939Names(const J1939NameTables* names);

    /// The loaded databases, as the DBC Explorer holds them. What a dropped signal is looked up in,
    /// what a widget's binding is checked against, and what a Table lists. Empty is a dashboard
    /// edited without one, which works exactly as it did before there was a dictionary.
    void setDatabases(const std::vector<std::shared_ptr<const CanDatabase>>& databases);

    [[nodiscard]] const SignalDictionary& dictionary() const noexcept { return m_dictionary; }

    /// The project's folder, which the pictures of the widgets are relative to. Empty for a project
    /// that has not been saved.
    void setAssetBase(const QString& directory);

    [[nodiscard]] const QString& assetBase() const noexcept { return m_images.baseDirectory(); }

    /// Rebuilds from the description - after a project is opened, or the
    /// dashboard is changed from outside.
    void reload();

    [[nodiscard]] bool isEditing() const noexcept { return m_editing; }

    // --- Making widgets ---------------------------------------------------------------------
    //
    // Public because a drop, the picker, the menu and a test all make them the same way, and a way
    // that is one function is a way that cannot be three slightly different ones.

    /// A widget for each signal, in a grid starting at `where`: the kind each wants, or `kind` when
    /// one was asked for. Returns the ids, selects the last and reports the edit.
    std::vector<std::string> addSignals(const std::vector<DictionaryEntry>& entries,
                                        std::optional<DashboardWidgetKind> kind,
                                        const QPoint& where);

    /// A table of every signal of `message`. Returns its id.
    std::string addTable(std::string_view message, const QPoint& where);

    /// An Image of the picture in `file`, as large as its own proportions make a sensible widget.
    /// Empty, and a line on the Output panel, when `file` is not a picture.
    std::string addImage(const QString& file, const QPoint& where);

    /// Binds the widget `id` to a signal and fills in what the database knows (applyEntry). False
    /// when the widget is not there or reads nothing a signal could be.
    bool bindWidget(const std::string& id, const DictionaryEntry& entry);

    /// Gives the widget `id` the picture in `file`. False when it has no use for one.
    bool setPicture(const std::string& id, const QString& file);

    /// Copies the selected widget. Returns the new id, which is then the selected one.
    std::string duplicateSelected();

    /// Selects a widget, as a click does. An id that is not there clears the selection.
    void select(const std::string& id);

    [[nodiscard]] const std::string& selectedId() const noexcept { return m_selected; }

    // --- Asking ---------------------------------------------------------------------------------

    /// What hovering at `point` says: the widget's name, what it is bound to, what it reads now,
    /// what the database knows of it and what is wrong with it. Empty over nothing.
    [[nodiscard]] QString detailsAt(const QPoint& point) const;

    /// The rows a Table shows now: its message's signals, with what each reads.
    [[nodiscard]] std::vector<TableRow> tableRowsOf(const std::string& widgetId) const;

    /// The rows every Faults widget lists now: one for each trouble code the bus is reporting,
    /// worst first. The bus's and not a widget's - a Faults widget is bound to nothing - so there
    /// is no widget to ask for. Empty when nothing is reported, which includes a measurement that
    /// is not running and a graph with no J1939 block.
    [[nodiscard]] std::vector<FaultRow> faultRows() const;

    /// The four lamps of the bus, as the J1939 block writes them to the system variables the
    /// cluster reads. All dark when there are no variables.
    [[nodiscard]] FaultLamps faultLamps() const;

    /// What is wrong with a widget's binding, as a sentence, or empty. Refreshed as the dashboard
    /// and the databases change.
    [[nodiscard]] QString findingFor(const std::string& widgetId) const;

    /// Whether a widget is showing nothing because its signal has gone quiet.
    [[nodiscard]] bool isStale(const std::string& widgetId) const
    {
        return m_stale.contains(widgetId);
    }

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
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool event(QEvent* event) override;

private Q_SLOTS:
    /// Repaints if anything a widget is bound to has moved. The panel's clock.
    void refresh();

private:
    using Reading = DashboardReading;
    using Clock = std::chrono::steady_clock;

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
    [[nodiscard]] const DashboardWidget* widgetAt(const QPoint& point) const;

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

    /// Opens the signal picker and adds what it returns at `where`: widgets for signals, tables for
    /// messages.
    void addFromPicker(const QPoint& where);

    /// Makes an Image of a file chosen with the file dialog.
    void addImageFromDialog(const QPoint& where);

    /// Puts `widget` into the description, selects it and reports the edit.
    std::string place(DashboardWidget widget);

    /// A drop in Edit mode that this panel can use: signals, or picture files.
    [[nodiscard]] bool canAccept(const QMimeData* data) const;

    /// The local picture files in a drop.
    [[nodiscard]] static QStringList pictureFilesIn(const QMimeData* data);

    /// Works out which widgets have gone silent, and forgets the ones that have been deleted.
    void updateStale(Clock::time_point now);

    /// Checks every binding against the dictionary again, if any binding has changed since the last
    /// time.
    void updateFindings();

    /// The recent samples of a Trend's signal or variable, or nullptr.
    [[nodiscard]] const SeriesWindow* trendWindowOf(const DashboardWidget& widget) const;

    /// The time the right edge of a Trend is: the bus clock for a signal, the panel's own for a
    /// variable.
    [[nodiscard]] std::uint64_t trendNewestNs(const DashboardWidget& widget) const;

    /// Adds this tick's value to the trail of every Trend bound to a variable, and drops what has
    /// fallen out of its window.
    void recordTrails(Clock::time_point now);

    /// Brings the codes of the bus up to date: nothing but an atomic load when the J1939 block has
    /// published nothing since the last time, a copy and a rebuild when it has. Const because
    /// painting and asking both need it and a cache is not part of what the panel is.
    void syncFaults() const;

    /// What hovering over a Faults widget adds: the lamps that are lit and each code in full.
    [[nodiscard]] QStringList faultDetails() const;

    /// The value the panel repaints a Faults widget for: the publication its rows are from and the
    /// lamps, either of which moving is a different picture.
    [[nodiscard]] std::uint64_t faultsToken() const;

    DashboardDescription& m_dashboard;

    /// A member, not a child: it has to go before the QQuickWidgets it made, which are children.
    /// ClusterHost explains why.
    std::unique_ptr<ClusterHost> m_clusters;

    const SignalSeriesStore* m_plots{nullptr};
    SystemVariables* m_variables{nullptr};

    const J1939Network* m_j1939{nullptr};
    const J1939NameTables* m_j1939Names{nullptr};

    /// The codes of the bus as the last publication had them, and as rows. Rebuilt by syncFaults()
    /// when the publication moves, so that a repaint twenty times a second does not copy a table.
    mutable std::vector<DashboardFault> m_faults;
    mutable std::vector<FaultRow> m_faultRows;

    /// The publication they are from, as the copy reported it.
    mutable std::uint64_t m_faultsRevision{0};

    /// False until they have been worked out for the network and the names now set.
    mutable bool m_faultsFresh{false};

    SignalDictionary m_dictionary;

    /// Counts the times the dictionary was rebuilt, so that a binding is checked again when the
    /// databases change under it and not only when the binding does.
    std::uint64_t m_dictionaryRevision{0};

    /// Mutable because asking whether a picture can be drawn is a read, and reading one may mean
    /// loading it: a cache is not part of what the panel is.
    mutable DashboardImages m_images;

    /// Timer ticks, for the checks that need not happen twenty times a second.
    unsigned m_ticks{0};

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

    /// The widget a drop would land on, drawn highlighted while it is dragged over.
    std::string m_dropTarget;

    /// The revisions the last repaint was drawn from, so a panel with nothing
    /// moving does not repaint twenty times a second.
    std::vector<std::uint64_t> m_lastRevisions;

    /// For each widget with a time-out: the last sample it saw change, and since when it has not.
    struct Seen final {
        std::uint64_t stamp{0};
        Clock::time_point since;
    };

    std::unordered_map<std::string, Seen> m_seen;
    std::unordered_set<std::string> m_stale;

    /// Widget id to what is wrong with its binding, and the bindings it was worked out from.
    std::unordered_map<std::string, QString> m_findings;
    std::string m_findingsFor;

    /// A scratch buffer for a Trend's samples, so that a repaint does not allocate one.
    mutable std::vector<SeriesWindow> m_windows;

    /// What a Trend bound to a variable has seen. A signal's history is in the plot store; a
    /// variable has none, so the panel keeps the last window of each - which means such a trend
    /// starts empty when the dashboard is shown, and fills as it is watched.
    std::unordered_map<std::string, std::deque<SignalSample>> m_trails;
    std::uint64_t m_trailsNowNs{0};
};

} // namespace torquebus::ui
