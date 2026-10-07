// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/DashboardPanel.h"

#include "core/dashboard/SystemVariables.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/plot/SignalSeries.h"
#include "ui/dashboard/cluster/ClusterHost.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QShowEvent>
#include <QTimer>

// For M_PI, which MSVC does not define from <cmath> without _USE_MATH_DEFINES.
// QtMath defines it if the platform did not, which is the portable way to ask.
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace torquebus::ui {
namespace {

/// 20 Hz, like every other panel that reads shared state on a timer. A needle
/// updated faster than the screen refreshes is work nobody can see.
constexpr int kRefreshMs = 50;

/// The resize handle in edit mode, in pixels.
constexpr double kHandleSize = 12.0;

/// A gauge sweeps 240 degrees, from lower-left round to lower-right, which is
/// what a vehicle instrument does and what makes the empty quarter at the
/// bottom read as "the scale stops here" rather than as a missing piece.
constexpr double kGaugeStartDegrees = 210.0;
constexpr double kGaugeSweepDegrees = -240.0;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// Where `value` sits between the bounds, clamped to 0..1.
[[nodiscard]] double fraction(double value, double minimum, double maximum)
{
    if (!(maximum > minimum)) {
        return 0.0;
    }

    return std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
}

} // namespace

DashboardPanel::DashboardPanel(DashboardDescription& dashboard, QWidget* parent)
    : QWidget{parent}
    , m_dashboard{dashboard}
    , m_clusters{std::make_unique<ClusterHost>(*this)}
{
    connect(m_clusters.get(), &ClusterHost::reported, this, &DashboardPanel::reported);

    // Delete has to reach the panel in edit mode, and a panel that cannot take
    // focus never sees a key.
    setFocusPolicy(Qt::StrongFocus);

    auto* timer = new QTimer(this);
    timer->setInterval(kRefreshMs);
    timer->setTimerType(Qt::CoarseTimer);
    connect(timer, &QTimer::timeout, this, &DashboardPanel::refresh);
    timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { update(); });
    }
}

DashboardPanel::~DashboardPanel() = default;

void DashboardPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_lastRevisions.clear();
    refresh();
}

void DashboardPanel::setPlotStore(const SignalSeriesStore* store)
{
    m_plots = store;
    update();
}

void DashboardPanel::setVariables(SystemVariables* variables)
{
    m_variables = variables;
    update();
}

void DashboardPanel::reload()
{
    m_selected.clear();
    m_pressed.clear();
    m_drag = Drag::None;

    Q_EMIT selectionChanged(QString{});

    syncClusters();
    update();
}

void DashboardPanel::setEditing(bool editing)
{
    if (m_editing == editing) {
        return;
    }

    m_editing = editing;

    // A drag in progress belongs to the mode it started in.
    m_drag = Drag::None;
    m_pressed.clear();

    if (!m_editing) {
        m_selected.clear();
        Q_EMIT selectionChanged(QString{});
    }

    syncClusters();
    update();
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

DashboardPanel::Reading DashboardPanel::read(const DashboardWidget& widget) const
{
    return readBinding(widget.binding);
}

DashboardPanel::Reading DashboardPanel::readBinding(const DashboardBinding& binding) const
{
    Reading reading;

    switch (binding.source) {
    case DashboardBinding::Source::Signal: {
        if (m_plots == nullptr) {
            break;
        }

        // "Message.Signal" is the store's own key, and the binding carries both
        // halves for exactly this.
        const std::string qualified = binding.message + "." + binding.signal;

        const SeriesId id = m_plots->find(qualified);

        std::uint64_t when = 0;
        reading.known = m_plots->latest(id, reading.value, when);
        break;
    }

    case DashboardBinding::Source::Variable:
        if (m_variables == nullptr) {
            break;
        }

        // Always known, unlike a signal: a variable nobody has written reads as
        // zero, and zero is its value rather than the absence of one.
        reading.value = m_variables->value(binding.variable);
        reading.known = true;
        break;

    case DashboardBinding::Source::None:
        break;
    }

    return reading;
}

void DashboardPanel::write(const DashboardWidget& widget, double value)
{
    if (widget.binding.source != DashboardBinding::Source::Variable || m_variables == nullptr) {
        return;
    }

    m_variables->set(widget.binding.variable, value);
}

void DashboardPanel::syncClusters()
{
    m_clusters->sync(m_dashboard, m_editing);
}

void DashboardPanel::refresh()
{
    if (!isVisible()) {
        return;
    }

    // Every tick, because the description can change without the panel being told: the widget
    // editor writes a cluster's profile straight into it. A tick with nothing changed does
    // nothing but compare rectangles.
    syncClusters();

    m_clusters->feed([this](const DashboardBinding& binding) -> std::optional<double> {
        const Reading reading = readBinding(binding);
        return reading.known ? std::optional<double>{reading.value} : std::nullopt;
    });

    // Repainted only when something a widget is bound to has moved. A dashboard
    // of eight gauges on a stopped measurement would otherwise repaint twenty
    // times a second to draw the same picture.
    std::vector<std::uint64_t> revisions;
    revisions.reserve(m_dashboard.widgets().size());

    for (const DashboardWidget& widget : m_dashboard.widgets()) {
        const Reading reading = read(widget);

        // The value itself, as bits, rather than a revision counter: a signal
        // has no counter, and this answers the same question for both sources.
        std::uint64_t token = 0;
        static_assert(sizeof(token) == sizeof(reading.value));
        std::memcpy(&token, &reading.value, sizeof(token));

        revisions.push_back(reading.known ? token : 0);
    }

    if (revisions != m_lastRevisions) {
        m_lastRevisions = std::move(revisions);
        update();
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

QRectF DashboardPanel::rectOf(const DashboardWidget& widget) const
{
    return QRectF{widget.x, widget.y, widget.width, widget.height};
}

QRectF DashboardPanel::handleOf(const DashboardWidget& widget) const
{
    const QRectF rect = rectOf(widget);

    return QRectF{
        rect.right() - kHandleSize, rect.bottom() - kHandleSize, kHandleSize, kHandleSize};
}

DashboardWidget* DashboardPanel::widgetAt(const QPoint& point)
{
    // Backwards: later widgets are drawn over earlier ones, so they are hit
    // first too. Anything else means the widget on top cannot be clicked.
    std::vector<DashboardWidget>& widgets = m_dashboard.widgets();

    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
        if (rectOf(*it).contains(point)) {
            return &*it;
        }
    }

    return nullptr;
}

double DashboardPanel::valueFromPoint(const DashboardWidget& widget,
                                      const QRectF& rect,
                                      const QPoint& point)
{
    const double span = widget.maximum - widget.minimum;

    if (widget.kind == DashboardWidgetKind::Knob) {
        // Angle from the centre, mapped onto the same sweep the knob is drawn
        // with. Not vertical drag distance: a knob that follows the finger
        // round is what a knob looks like it should do.
        const QPointF centre = rect.center();
        const double dx = point.x() - centre.x();
        const double dy = point.y() - centre.y();

        double degrees = std::atan2(-dy, dx) * 180.0 / M_PI;

        // Into the gauge's own frame: 210 degrees down to -30.
        if (degrees > kGaugeStartDegrees) {
            degrees -= 360.0;
        }
        if (degrees < kGaugeStartDegrees + kGaugeSweepDegrees) {
            degrees += 360.0;
        }

        const double swept = (kGaugeStartDegrees - degrees) / -kGaugeSweepDegrees;

        return widget.minimum + std::clamp(swept, 0.0, 1.0) * span;
    }

    // A slider: across if it is wider than tall, up if it is taller. Deciding
    // from the shape rather than from a setting, because a vertical slider is
    // drawn vertically and asking for the orientation twice is a way for the
    // two answers to disagree.
    if (rect.width() >= rect.height()) {
        const double along = (point.x() - rect.left()) / std::max(1.0, rect.width());
        return widget.minimum + std::clamp(along, 0.0, 1.0) * span;
    }

    const double along = (rect.bottom() - point.y()) / std::max(1.0, rect.height());
    return widget.minimum + std::clamp(along, 0.0, 1.0) * span;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void DashboardPanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPoint point = event->pos();
    DashboardWidget* widget = widgetAt(point);

    if (m_editing) {
        m_selected = widget != nullptr ? widget->id : std::string{};
        Q_EMIT selectionChanged(QString::fromStdString(m_selected));

        if (widget != nullptr) {
            m_dragFrom = point;
            m_dragOrigin = rectOf(*widget);

            // The corner first: it is inside the widget, so a test in the other
            // order would always find the move.
            m_drag = handleOf(*widget).contains(point) ? Drag::Resize : Drag::Move;
        } else {
            m_drag = Drag::None;
        }

        update();
        return;
    }

    // --- Run mode: the click operates the control ---------------------------
    if (widget == nullptr || !writesItsBinding(widget->kind)) {
        return;
    }

    const QRectF rect = rectOf(*widget);

    switch (widget->kind) {
    case DashboardWidgetKind::Button:
        // Pressed now, released on mouse-up wherever that happens: a momentary
        // contact that stayed closed because the finger slid off the button is
        // not what the hardware does.
        m_pressed = widget->id;
        write(*widget, widget->maximum);
        break;

    case DashboardWidgetKind::Switch: {
        const Reading reading = read(*widget);
        const bool on = reading.value >= widget->threshold;

        write(*widget, on ? widget->minimum : widget->maximum);
        break;
    }

    case DashboardWidgetKind::Slider:
    case DashboardWidgetKind::Knob:
        m_drag = Drag::Operate;
        m_pressed = widget->id;
        write(*widget, valueFromPoint(*widget, rect, point));
        break;

    default:
        break;
    }

    update();
}

void DashboardPanel::mouseMoveEvent(QMouseEvent* event)
{
    if (m_drag == Drag::None) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint point = event->pos();

    if (m_drag == Drag::Operate) {
        const DashboardWidget* widget = m_dashboard.find(m_pressed);

        if (widget != nullptr) {
            write(*widget, valueFromPoint(*widget, rectOf(*widget), point));
            update();
        }

        return;
    }

    DashboardWidget* widget = const_cast<DashboardWidget*>(m_dashboard.find(m_selected));
    if (widget == nullptr) {
        return;
    }

    const QPoint delta = point - m_dragFrom;

    if (m_drag == Drag::Move) {
        widget->x = m_dragOrigin.left() + delta.x();
        widget->y = m_dragOrigin.top() + delta.y();
    } else {
        // A floor rather than a clamp to the panel: a widget dragged smaller
        // than this stops being clickable, which is how one gets lost.
        widget->width = std::max(40.0, m_dragOrigin.width() + delta.x());
        widget->height = std::max(30.0, m_dragOrigin.height() + delta.y());
    }

    syncClusters();
    update();
}

void DashboardPanel::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (!m_pressed.empty()) {
        if (const DashboardWidget* widget = m_dashboard.find(m_pressed);
            widget != nullptr && widget->kind == DashboardWidgetKind::Button) {
            write(*widget, widget->minimum);
        }
    }

    // One edit, reported once, when the mouse comes up - not per mouse-move.
    // Otherwise dragging a gauge across the panel would mark the project dirty
    // two hundred times and fill the undo history nobody has yet.
    if (m_drag == Drag::Move || m_drag == Drag::Resize) {
        Q_EMIT dashboardEdited();
    }

    m_drag = Drag::None;
    m_pressed.clear();

    update();
}

void DashboardPanel::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_editing) {
        // Nothing to offer in run mode. A context menu that only says "you
        // cannot do anything here" is worse than none.
        return;
    }

    QMenu menu{this};

    const QPoint where = event->pos();

    const auto addKind = [this, &menu, where](DashboardWidgetKind kind, const QString& label) {
        QAction* action = menu.addAction(label);
        connect(action, &QAction::triggered, this, [this, kind, where] { addWidget(kind, where); });
    };

    addKind(DashboardWidgetKind::Gauge, tr("Add Gauge"));
    addKind(DashboardWidgetKind::Numeric, tr("Add Numeric"));
    addKind(DashboardWidgetKind::Lamp, tr("Add Lamp"));
    menu.addSeparator();
    addKind(DashboardWidgetKind::Slider, tr("Add Slider"));
    addKind(DashboardWidgetKind::Knob, tr("Add Knob"));
    addKind(DashboardWidgetKind::Switch, tr("Add Switch"));
    addKind(DashboardWidgetKind::Button, tr("Add Button"));
    menu.addSeparator();
    addKind(DashboardWidgetKind::Label, tr("Add Label"));
    menu.addSeparator();
    addKind(DashboardWidgetKind::Cluster, tr("Add Cluster"));

    if (const DashboardWidget* widget = widgetAt(where); widget != nullptr) {
        menu.addSeparator();

        QAction* remove = menu.addAction(tr("Delete '%1'").arg(QString::fromStdString(widget->id)));

        const std::string id = widget->id;
        connect(remove, &QAction::triggered, this, [this, id] {
            m_selected = id;
            removeSelected();
        });
    }

    menu.exec(event->globalPos());
}

void DashboardPanel::keyPressEvent(QKeyEvent* event)
{
    if (m_editing && !m_selected.empty()
        && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        removeSelected();
        return;
    }

    QWidget::keyPressEvent(event);
}

void DashboardPanel::addWidget(DashboardWidgetKind kind, const QPoint& where)
{
    DashboardWidget widget;
    widget.id = m_dashboard.uniqueId(std::string{nameOf(kind)});
    widget.kind = kind;
    widget.x = where.x();
    widget.y = where.y();

    // A control needs a variable to write and a reader needs something to read.
    // Neither is guessed: the widget arrives unbound and validate() says so,
    // which is the same contract a half-configured block on the canvas has.
    switch (kind) {
    case DashboardWidgetKind::Slider:
        widget.width = 220.0;
        widget.height = 60.0;
        widget.maximum = 1.0;
        break;

    case DashboardWidgetKind::Knob:
        widget.width = 140.0;
        widget.height = 140.0;
        widget.maximum = 1.0;
        break;

    case DashboardWidgetKind::Lamp:
    case DashboardWidgetKind::Button:
    case DashboardWidgetKind::Switch:
        widget.width = 120.0;
        widget.height = 80.0;
        widget.maximum = 1.0;
        break;

    case DashboardWidgetKind::Label:
        widget.width = 200.0;
        widget.height = 40.0;
        widget.title = "Label";
        break;

    case DashboardWidgetKind::Cluster:
        // The cluster's own stage is 1280 x 560; drawn at another shape it is centred in what it
        // is given, so this is just the size at which it is first seen whole.
        widget.width = 720.0;
        widget.height = 315.0;
        widget.profile = std::string{kDefaultClusterProfile};
        break;

    default:
        break;
    }

    m_selected = widget.id;
    m_dashboard.add(std::move(widget));

    Q_EMIT selectionChanged(QString::fromStdString(m_selected));
    Q_EMIT dashboardEdited();

    syncClusters();
    update();
}

void DashboardPanel::removeSelected()
{
    if (m_selected.empty()) {
        return;
    }

    m_dashboard.remove(m_selected);
    m_selected.clear();

    Q_EMIT selectionChanged(QString{});
    Q_EMIT dashboardEdited();

    syncClusters();
    update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

QString DashboardPanel::captionOf(const DashboardWidget& widget) const
{
    if (!widget.title.empty()) {
        return QString::fromStdString(widget.title);
    }

    switch (widget.binding.source) {
    case DashboardBinding::Source::Signal:
        return QString::fromStdString(widget.binding.signal);
    case DashboardBinding::Source::Variable:
        return QString::fromStdString(widget.binding.variable);
    case DashboardBinding::Source::None:
        break;
    }

    // Unbound, and in edit mode that is the thing worth saying.
    return tr("(not bound)");
}

void DashboardPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    const Theme theme = currentTheme();

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The deepest surface in the theme, like the node canvas: a dashboard is
    // looked at in a dark room beside a vehicle as often as at a desk.
    painter.fillRect(rect(), theme.canvas);

    if (m_editing) {
        // A grid, and only in edit mode. It is an alignment aid, and a bench
        // instrument with graph paper behind it looks unfinished.
        QPen grid{theme.canvasGridFine};
        grid.setWidthF(1.0);
        painter.setPen(grid);

        constexpr int kStep = 20;
        for (int x = 0; x < width(); x += kStep) {
            painter.drawLine(x, 0, x, height());
        }
        for (int y = 0; y < height(); y += kStep) {
            painter.drawLine(0, y, width(), y);
        }
    }

    if (m_dashboard.empty()) {
        painter.setPen(theme.textMuted);
        painter.drawText(rect(),
                         Qt::AlignCenter,
                         m_editing ? tr("Right-click to add a gauge, a lamp or a slider.")
                                   : tr("This project has no dashboard yet.\n"
                                        "Switch to Edit and right-click to build one."));
        return;
    }

    for (const DashboardWidget& widget : m_dashboard.widgets()) {
        paintWidget(painter, widget);
    }
}

void DashboardPanel::paintWidget(QPainter& painter, const DashboardWidget& widget)
{
    const Theme theme = currentTheme();
    const QRectF rect = rectOf(widget);
    const Reading reading = read(widget);

    painter.save();

    switch (widget.kind) {
    case DashboardWidgetKind::Gauge:
        paintGauge(painter, widget, rect, reading);
        break;
    case DashboardWidgetKind::Numeric:
        paintNumeric(painter, widget, rect, reading);
        break;
    case DashboardWidgetKind::Lamp:
        paintLamp(painter, widget, rect, reading);
        break;
    case DashboardWidgetKind::Slider:
        paintSlider(painter, widget, rect, reading);
        break;
    case DashboardWidgetKind::Knob:
        paintKnob(painter, widget, rect, reading);
        break;
    case DashboardWidgetKind::Button:
    case DashboardWidgetKind::Switch:
        paintButton(painter, widget, rect, reading);
        break;
    case DashboardWidgetKind::Label:
        paintLabel(painter, widget, rect);
        break;
    case DashboardWidgetKind::Cluster:
        paintCluster(painter, widget, rect);
        break;
    }

    painter.restore();

    if (!m_editing) {
        return;
    }

    // --- Edit chrome -------------------------------------------------------
    const bool selected = widget.id == m_selected;

    QPen outline{selected ? theme.accent : theme.border};
    outline.setWidthF(selected ? 2.0 : 1.0);
    outline.setStyle(selected ? Qt::SolidLine : Qt::DashLine);

    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect);

    if (selected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.accent);
        painter.drawRect(handleOf(widget));
    }
}

void DashboardPanel::paintGauge(QPainter& painter,
                                const DashboardWidget& widget,
                                const QRectF& rect,
                                const Reading& reading)
{
    const Theme theme = currentTheme();

    const double side = std::min(rect.width(), rect.height() * 1.25);
    QRectF face{0.0, 0.0, side * 0.8, side * 0.8};
    face.moveCenter(QPointF{rect.center().x(), rect.center().y() - rect.height() * 0.05});

    // The track, then the swept part over it: two arcs rather than a filled pie,
    // because an instrument reads as a scale and not as a chart.
    QPen track{theme.border};
    track.setWidthF(std::max(4.0, side * 0.06));
    track.setCapStyle(Qt::FlatCap);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(track);
    painter.drawArc(
        face, static_cast<int>(kGaugeStartDegrees * 16), static_cast<int>(kGaugeSweepDegrees * 16));

    if (reading.known) {
        const double swept = fraction(reading.value, widget.minimum, widget.maximum);

        QPen live{theme.accent};
        live.setWidthF(track.widthF());
        live.setCapStyle(Qt::FlatCap);
        painter.setPen(live);
        painter.drawArc(face,
                        static_cast<int>(kGaugeStartDegrees * 16),
                        static_cast<int>(kGaugeSweepDegrees * swept * 16));

        // The needle. Drawn from the centre so that a gauge resized by its
        // corner keeps its proportions rather than growing a longer needle.
        const double degrees = kGaugeStartDegrees + kGaugeSweepDegrees * swept;
        const double radians = degrees * M_PI / 180.0;
        const double length = face.width() * 0.42;

        const QPointF centre = face.center();
        const QPointF tip{centre.x() + std::cos(radians) * length,
                          centre.y() - std::sin(radians) * length};

        QPen needle{theme.text};
        needle.setWidthF(std::max(2.0, side * 0.02));
        needle.setCapStyle(Qt::RoundCap);
        painter.setPen(needle);
        painter.drawLine(centre, tip);
    }

    // The number under the needle: a gauge is read for the shape of the change
    // and the number is read for the value, and both are wanted at once.
    painter.setPen(reading.known ? theme.text : theme.textMuted);

    const QString value = reading.known
                              ? QStringLiteral("%1").arg(reading.value, 0, 'f', widget.decimals)
                              : QStringLiteral("--");

    const QString unit = QString::fromStdString(widget.unit);

    painter.drawText(QRectF{rect.left(),
                            rect.center().y() + rect.height() * 0.18,
                            rect.width(),
                            rect.height() * 0.2},
                     Qt::AlignCenter,
                     unit.isEmpty() ? value : value + QStringLiteral(" ") + unit);

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{rect.left(), rect.bottom() - rect.height() * 0.2, rect.width(), rect.height() * 0.2},
        Qt::AlignCenter,
        captionOf(widget));
}

void DashboardPanel::paintNumeric(QPainter& painter,
                                  const DashboardWidget& widget,
                                  const QRectF& rect,
                                  const Reading& reading)
{
    const Theme theme = currentTheme();

    painter.setPen(theme.border);
    painter.setBrush(theme.panel);
    painter.drawRoundedRect(rect, 4.0, 4.0);

    const QString value = reading.known
                              ? QStringLiteral("%1").arg(reading.value, 0, 'f', widget.decimals)
                              : QStringLiteral("--");

    const QString unit = QString::fromStdString(widget.unit);

    QFont big = painter.font();

    // Scaled to the box rather than fixed: the whole point of a numeric readout
    // is that it can be read from across a workshop, which means it has to grow
    // when somebody makes the box bigger.
    big.setPointSizeF(std::max(10.0, rect.height() * 0.35));
    big.setBold(true);
    painter.setFont(big);

    painter.setPen(reading.known ? theme.text : theme.textMuted);
    painter.drawText(
        QRectF{rect.left(), rect.top() + rect.height() * 0.1, rect.width(), rect.height() * 0.55},
        Qt::AlignCenter,
        unit.isEmpty() ? value : value + QStringLiteral(" ") + unit);

    QFont small = painter.font();
    small.setPointSizeF(std::max(8.0, rect.height() * 0.16));
    small.setBold(false);
    painter.setFont(small);

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.3, rect.width(), rect.height() * 0.25},
        Qt::AlignCenter,
        captionOf(widget));
}

void DashboardPanel::paintLamp(QPainter& painter,
                               const DashboardWidget& widget,
                               const QRectF& rect,
                               const Reading& reading)
{
    const Theme theme = currentTheme();

    const bool on = reading.known && reading.value >= widget.threshold;

    const double diameter = std::min(rect.width(), rect.height() * 0.6) * 0.7;
    QRectF bulb{0.0, 0.0, diameter, diameter};
    bulb.moveCenter(QPointF{rect.center().x(), rect.top() + rect.height() * 0.4});

    // Off is a dim outline rather than a different colour: a lamp that is off
    // still has to look like the lamp that was on, or nobody believes it is the
    // same one.
    QColor fill = theme.warning;
    if (!on) {
        fill.setAlpha(40);
    }

    painter.setBrush(fill);
    painter.setPen(QPen{on ? theme.warning : theme.border, 2.0});
    painter.drawEllipse(bulb);

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.3, rect.width(), rect.height() * 0.25},
        Qt::AlignCenter,
        captionOf(widget));
}

void DashboardPanel::paintSlider(QPainter& painter,
                                 const DashboardWidget& widget,
                                 const QRectF& rect,
                                 const Reading& reading)
{
    const Theme theme = currentTheme();

    const bool horizontal = rect.width() >= rect.height();
    const double swept = fraction(reading.value, widget.minimum, widget.maximum);

    QRectF groove = rect;

    if (horizontal) {
        groove.setHeight(10.0);
        groove.moveTop(rect.top() + rect.height() * 0.35);
        groove.adjust(8.0, 0.0, -8.0, 0.0);
    } else {
        groove.setWidth(10.0);
        groove.moveLeft(rect.center().x() - 5.0);
        groove.adjust(0.0, 8.0, 0.0, -8.0);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.border);
    painter.drawRoundedRect(groove, 5.0, 5.0);

    QRectF filled = groove;
    if (horizontal) {
        filled.setWidth(groove.width() * swept);
    } else {
        filled.setTop(groove.bottom() - groove.height() * swept);
    }

    painter.setBrush(theme.accent);
    painter.drawRoundedRect(filled, 5.0, 5.0);

    // The handle, which is what says this is a thing to be moved rather than a
    // progress bar.
    const QPointF centre =
        horizontal ? QPointF{groove.left() + groove.width() * swept, groove.center().y()}
                   : QPointF{groove.center().x(), groove.bottom() - groove.height() * swept};

    painter.setBrush(theme.text);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(centre, 9.0, 9.0);

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.3, rect.width(), rect.height() * 0.28},
        Qt::AlignCenter,
        QStringLiteral("%1  %2")
            .arg(captionOf(widget))
            .arg(reading.value, 0, 'f', widget.decimals));
}

void DashboardPanel::paintKnob(QPainter& painter,
                               const DashboardWidget& widget,
                               const QRectF& rect,
                               const Reading& reading)
{
    const Theme theme = currentTheme();

    const double diameter = std::min(rect.width(), rect.height() * 0.75) * 0.8;
    QRectF body{0.0, 0.0, diameter, diameter};
    body.moveCenter(QPointF{rect.center().x(), rect.top() + rect.height() * 0.42});

    painter.setBrush(theme.panel);
    painter.setPen(QPen{theme.border, 2.0});
    painter.drawEllipse(body);

    const double swept = fraction(reading.value, widget.minimum, widget.maximum);
    const double degrees = kGaugeStartDegrees + kGaugeSweepDegrees * swept;
    const double radians = degrees * M_PI / 180.0;

    const QPointF centre = body.center();
    const QPointF tip{centre.x() + std::cos(radians) * diameter * 0.38,
                      centre.y() - std::sin(radians) * diameter * 0.38};

    painter.setPen(QPen{theme.accent, 3.0, Qt::SolidLine, Qt::RoundCap});
    painter.drawLine(centre, tip);

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.26, rect.width(), rect.height() * 0.24},
        Qt::AlignCenter,
        QStringLiteral("%1  %2")
            .arg(captionOf(widget))
            .arg(reading.value, 0, 'f', widget.decimals));
}

void DashboardPanel::paintButton(QPainter& painter,
                                 const DashboardWidget& widget,
                                 const QRectF& rect,
                                 const Reading& reading)
{
    const Theme theme = currentTheme();

    const bool on = reading.known && reading.value >= widget.threshold;

    QRectF face = rect.adjusted(6.0, 6.0, -6.0, -rect.height() * 0.3);

    painter.setPen(QPen{on ? theme.accent : theme.border, 2.0});
    painter.setBrush(on ? theme.accent : theme.panel);
    painter.drawRoundedRect(face, 6.0, 6.0);

    painter.setPen(on ? theme.textInverted : theme.text);
    painter.drawText(face,
                     Qt::AlignCenter,
                     widget.kind == DashboardWidgetKind::Switch ? (on ? tr("ON") : tr("OFF"))
                                                                : captionOf(widget));

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.28, rect.width(), rect.height() * 0.26},
        Qt::AlignCenter,
        captionOf(widget));
}

void DashboardPanel::paintLabel(QPainter& painter,
                                const DashboardWidget& widget,
                                const QRectF& rect)
{
    const Theme theme = currentTheme();

    QFont font = painter.font();
    font.setPointSizeF(std::max(10.0, rect.height() * 0.45));
    font.setBold(true);
    painter.setFont(font);

    painter.setPen(theme.text);
    painter.drawText(rect, Qt::AlignCenter, QString::fromStdString(widget.title));
}

void DashboardPanel::paintCluster(QPainter& painter,
                                  const DashboardWidget& widget,
                                  const QRectF& rect)
{
    const QString problem = m_clusters->problem();

    // In Run mode the QML is on top of this, and what is painted here is only what shows through
    // while it loads. Unless it never will: then this is where it says so.
    if (!m_editing && problem.isEmpty()) {
        return;
    }

    const Theme theme = currentTheme();

    // The picture the cluster left when Edit mode took it off the screen, so that a cluster being
    // placed and sized looks like one. Fitted, not stretched: the cluster itself keeps its shape in
    // whatever rectangle it is given.
    if (const QImage* snapshot = m_editing ? m_clusters->snapshotOf(widget.id) : nullptr;
        snapshot != nullptr) {
        const QSizeF size = QSizeF{snapshot->size()}.scaled(rect.size(), Qt::KeepAspectRatio);
        QRectF target{QPointF{}, size};
        target.moveCenter(rect.center());

        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(target, *snapshot);
        return;
    }

    // Nothing to show yet - a cluster added in Edit mode, or one that cannot load.
    painter.setPen(QPen{theme.border, 1.0});
    painter.setBrush(theme.instrumentBezel);
    painter.drawRoundedRect(rect.adjusted(1.0, 1.0, -1.0, -1.0), 10.0, 10.0);

    QString text = problem;

    if (text.isEmpty()) {
        const ClusterProfile* profile = ClusterProfiles::instance().find(widget.profile);
        text = tr("Instrument cluster\n%1")
                   .arg(profile != nullptr ? QString::fromStdString(profile->name)
                                           : QString::fromStdString(widget.profile));
    }

    painter.setPen(theme.textMuted);
    painter.drawText(
        rect.adjusted(12.0, 12.0, -12.0, -12.0), Qt::AlignCenter | Qt::TextWordWrap, text);
}

} // namespace torquebus::ui
