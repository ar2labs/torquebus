// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/DashboardPanel.h"

#include "core/dashboard/DashboardAssets.h"
#include "core/dashboard/SystemVariables.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/plot/SignalSeries.h"
#include "ui/dashboard/DashboardText.h"
#include "ui/dashboard/SignalMime.h"
#include "ui/dashboard/SignalPickerDialog.h"
#include "ui/dashboard/cluster/ClusterHost.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QShowEvent>
#include <QTimer>
#include <QToolTip>
#include <QUrl>

// For M_PI, which MSVC does not define from <cmath> without _USE_MATH_DEFINES.
// QtMath defines it if the platform did not, which is the portable way to ask.
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>

namespace torquebus::ui {
namespace {

/// `text` cut to fit `width` pixels in the painter's current font, ending in an ellipsis when it
/// had to be. A title from a database is as long as the database made it - "Driver Demand Torque"
/// in a box the size of a number - and text that runs past its box is clipped at both ends, which
/// reads as a different word.
[[nodiscard]] QString fitted(const QPainter& painter, const QString& text, double width)
{
    return painter.fontMetrics().elidedText(
        text, Qt::ElideRight, static_cast<int>(std::max(0.0, width - 6.0)));
}

/// 20 Hz, like every other panel that reads shared state on a timer. A needle
/// updated faster than the screen refreshes is work nobody can see.
constexpr int kRefreshMs = 50;

/// How many ticks between the checks that a picture on disk is still the one on screen: two
/// seconds.
constexpr unsigned kPictureRecheckTicks = 40;

/// The resize handle in edit mode, in pixels.
constexpr double kHandleSize = 12.0;

/// A gauge sweeps 240 degrees, from lower-left round to lower-right, which is
/// what a vehicle instrument does and what makes the empty quarter at the
/// bottom read as "the scale stops here" rather than as a missing piece.
constexpr double kGaugeStartDegrees = 210.0;
constexpr double kGaugeSweepDegrees = -240.0;

/// The gap between widgets made together from several signals.
constexpr int kGridGap = 12;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// The value as the bits it is made of: a signal has no revision counter, and this answers the same
/// question for both sources - did it change.
[[nodiscard]] std::uint64_t bitsOf(double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t seed, std::uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

/// Whether a signal can be bound to this kind of widget: one that reads, and has a value to read.
[[nodiscard]] bool takesASignal(const DashboardWidget& widget)
{
    const BindingNeed need = traitsOf(widget.kind).binding;

    return !writesItsBinding(widget.kind)
           && (need == BindingNeed::Required || need == BindingNeed::Optional);
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

    // Signals and pictures are dropped on it, in Edit mode; in Run mode canAccept says no.
    setAcceptDrops(true);
    setMouseTracking(true);

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

void DashboardPanel::setJ1939Network(const J1939Network* network)
{
    m_j1939 = network;
    m_faultsFresh = false;
    update();
}

void DashboardPanel::setJ1939Names(const J1939NameTables* names)
{
    m_j1939Names = names;
    m_faultsFresh = false;
    update();
}

void DashboardPanel::setDatabases(const std::vector<std::shared_ptr<const CanDatabase>>& databases)
{
    m_dictionary.rebuild(databases);

    // The bindings are checked against a different dictionary now.
    ++m_dictionaryRevision;
    m_findingsFor.clear();

    update();
}

void DashboardPanel::setAssetBase(const QString& directory)
{
    m_images.setBaseDirectory(directory);
    update();
}

void DashboardPanel::reload()
{
    m_selected.clear();
    m_pressed.clear();
    m_dropTarget.clear();
    m_drag = Drag::None;

    m_seen.clear();
    m_stale.clear();
    m_findings.clear();
    m_findingsFor.clear();

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
    m_dropTarget.clear();

    if (!m_editing) {
        m_selected.clear();
        Q_EMIT selectionChanged(QString{});
    }

    syncClusters();
    update();
}

void DashboardPanel::select(const std::string& id)
{
    m_selected = m_dashboard.find(id) != nullptr ? id : std::string{};

    Q_EMIT selectionChanged(QString::fromStdString(m_selected));
    update();
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

DashboardPanel::Reading DashboardPanel::read(const DashboardWidget& widget) const
{
    Reading reading = readBinding(widget.binding);

    // A signal that has been silent for longer than the widget allows is not shown as the value it
    // last had: dashes, and a badge that says why.
    if (m_stale.contains(widget.id)) {
        reading.known = false;
        reading.timed = false;
        reading.stale = true;
    }

    return reading;
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

        // A J1939 signal that reports "error" or "not available" is a sample whose value is NaN,
        // and a signal that has nothing to report has no value: dashes on a Numeric, an empty
        // Gauge, a Lamp that is off. Drawn as a number it is an arc swept by an undefined amount -
        // NaN cast to an integer - and "nan" where the reading goes.
        if (reading.known && !std::isfinite(reading.value)) {
            reading.known = false;
        }

        reading.stamp = when;
        reading.timed = reading.known;
        break;
    }

    case DashboardBinding::Source::Variable:
        if (m_variables == nullptr) {
            break;
        }

        // Known whenever it is a number, unlike a signal: a variable nobody has
        // written reads as zero, and zero is its value rather than the absence
        // of one.
        reading.value = m_variables->value(binding.variable);
        reading.known = std::isfinite(reading.value);
        break;

    // A whole message has no single value; a Table asks for each of its signals.
    case DashboardBinding::Source::Message:
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

void DashboardPanel::updateStale(Clock::time_point now)
{
    std::unordered_set<std::string> stale;

    for (const DashboardWidget& widget : m_dashboard.widgets()) {
        if (widget.staleMs == 0 || widget.binding.source != DashboardBinding::Source::Signal) {
            m_seen.erase(widget.id);
            continue;
        }

        // The raw reading, not read(): this is what decides whether it is stale.
        const Reading raw = readBinding(widget.binding);

        // A signal that never spoke, or whose last word was "not available", is unknown and not
        // stale: stale is a signal that was alive.
        if (!raw.known || !raw.timed) {
            m_seen.erase(widget.id);
            continue;
        }

        const auto [it, inserted] = m_seen.try_emplace(widget.id, Seen{raw.stamp, now});
        Seen& seen = it->second;

        if (!inserted && seen.stamp != raw.stamp) {
            // A new sample, whatever its stamp: compared for change and not for order, so a store
            // that started over is as fresh as one that went on.
            seen = Seen{raw.stamp, now};
        } else if (now - seen.since > std::chrono::milliseconds{widget.staleMs}) {
            stale.insert(widget.id);
        }
    }

    // A widget that has been deleted leaves nothing behind.
    std::erase_if(m_seen,
                  [this](const auto& item) { return m_dashboard.find(item.first) == nullptr; });

    m_stale = std::move(stale);
}

void DashboardPanel::updateFindings()
{
    // The bindings and the dictionary they are checked against, as one string: cheap to build every
    // tick, and the only things the findings depend on.
    std::string signature = std::to_string(m_dictionaryRevision);

    for (const DashboardWidget& widget : m_dashboard.widgets()) {
        if (widget.binding.source == DashboardBinding::Source::Signal
            || widget.binding.source == DashboardBinding::Source::Message) {
            signature += '\n';
            signature += widget.id;
            signature += widget.binding.source == DashboardBinding::Source::Signal ? '|' : '#';
            signature += widget.binding.message;
            signature += '.';
            signature += widget.binding.signal;
        }
    }

    if (signature == m_findingsFor) {
        return;
    }

    m_findingsFor = std::move(signature);
    m_findings.clear();

    for (const WidgetFinding& finding : m_dictionary.audit(m_dashboard)) {
        if (const DashboardWidget* widget = m_dashboard.find(finding.widgetId); widget != nullptr) {
            m_findings[finding.widgetId] = describeFinding(finding.finding, widget->binding);
        }
    }

    update();
}

QString DashboardPanel::findingFor(const std::string& widgetId) const
{
    const auto found = m_findings.find(widgetId);
    return found == m_findings.end() ? QString{} : found->second;
}

void DashboardPanel::refresh()
{
    if (!isVisible()) {
        return;
    }

    const Clock::time_point now = Clock::now();

    updateStale(now);
    recordTrails(now);
    updateFindings();

    // Every tick, because the description can change without the panel being told: the widget
    // editor writes a cluster's profile straight into it. A tick with nothing changed does
    // nothing but compare rectangles.
    syncClusters();

    m_clusters->feed([this](const DashboardBinding& binding) -> std::optional<ClusterReading> {
        const Reading reading = readBinding(binding);

        if (!reading.known) {
            return std::nullopt;
        }

        return ClusterReading{reading.value,
                              reading.timed ? std::optional<std::uint64_t>{reading.stamp}
                                            : std::nullopt};
    });

    // Repainted only when something a widget is bound to has moved. A dashboard
    // of eight gauges on a stopped measurement would otherwise repaint twenty
    // times a second to draw the same picture.
    std::vector<std::uint64_t> revisions;
    revisions.reserve(m_dashboard.widgets().size() + 1);

    bool hasPictures = false;

    for (const DashboardWidget& widget : m_dashboard.widgets()) {
        const Reading reading = read(widget);

        // The value itself, as bits, rather than a revision counter: a signal
        // has no counter, and this answers the same question for both sources. A stale reading
        // is a different picture from an unknown one, so it is a different token.
        std::uint64_t token = reading.known ? bitsOf(reading.value) : (reading.stale ? 1U : 0U);

        if (widget.kind == DashboardWidgetKind::Trend) {
            // Its line moves with the clock, whether or not its own signal changed.
            token = mix(token, trendNewestNs(widget));
        } else if (widget.kind == DashboardWidgetKind::Faults) {
            // It has no binding to read: what it draws is the bus's, and moves with a publication
            // of the J1939 block or with a lamp.
            token = mix(token, faultsToken());
        } else if (widget.kind == DashboardWidgetKind::Table) {
            for (const DictionaryEntry* entry : m_dictionary.signalsOf(widget.binding.message)) {
                DashboardBinding row;
                row.source = DashboardBinding::Source::Signal;
                row.message = entry->message;
                row.signal = entry->signal;

                const Reading value = readBinding(row);
                token = mix(token, value.known ? bitsOf(value.value) : 0U);
            }
        }

        hasPictures = hasPictures || !widget.image.empty() || !widget.states.empty();
        revisions.push_back(token);
    }

    // A mark appearing or going is a change too.
    revisions.push_back(m_findings.size());

    if (revisions != m_lastRevisions) {
        m_lastRevisions = std::move(revisions);
        update();
    }

    // A picture saved over in an editor shows within a couple of seconds, on a dashboard that is
    // otherwise standing still.
    if (hasPictures && ++m_ticks % kPictureRecheckTicks == 0) {
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

const DashboardWidget* DashboardPanel::widgetAt(const QPoint& point) const
{
    const std::vector<DashboardWidget>& widgets = m_dashboard.widgets();

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

    // First, because it is the way to make a widget that is bound and configured when it arrives.
    QAction* fromDatabase = menu.addAction(tr("Add from Database..."));
    fromDatabase->setToolTip(tr("Pick signals, or a whole message, from the loaded databases. Each "
                                "becomes the widget it suits, already bound."));
    connect(fromDatabase, &QAction::triggered, this, [this, where] { addFromPicker(where); });
    menu.addSeparator();

    addKind(DashboardWidgetKind::Gauge, tr("Add Gauge"));
    addKind(DashboardWidgetKind::Numeric, tr("Add Numeric"));
    addKind(DashboardWidgetKind::Bar, tr("Add Bar"));
    addKind(DashboardWidgetKind::Lamp, tr("Add Lamp"));
    addKind(DashboardWidgetKind::State, tr("Add State"));
    addKind(DashboardWidgetKind::Trend, tr("Add Trend"));
    addKind(DashboardWidgetKind::Table, tr("Add Message Table"));
    addKind(DashboardWidgetKind::Faults, tr("Add Faults"));
    menu.addSeparator();
    addKind(DashboardWidgetKind::Slider, tr("Add Slider"));
    addKind(DashboardWidgetKind::Knob, tr("Add Knob"));
    addKind(DashboardWidgetKind::Switch, tr("Add Switch"));
    addKind(DashboardWidgetKind::Button, tr("Add Button"));
    menu.addSeparator();
    addKind(DashboardWidgetKind::Label, tr("Add Label"));
    addKind(DashboardWidgetKind::Group, tr("Add Group"));

    QAction* image = menu.addAction(tr("Add Image..."));
    connect(image, &QAction::triggered, this, [this, where] { addImageFromDialog(where); });

    menu.addSeparator();
    addKind(DashboardWidgetKind::Cluster, tr("Add Cluster"));

    if (const DashboardWidget* widget = widgetAt(where); widget != nullptr) {
        menu.addSeparator();

        const std::string id = widget->id;
        const QString name = QString::fromStdString(id);

        QAction* duplicate = menu.addAction(tr("Duplicate '%1'").arg(name));
        connect(duplicate, &QAction::triggered, this, [this, id] {
            m_selected = id;
            (void)duplicateSelected();
        });

        QAction* front = menu.addAction(tr("Bring '%1' to Front").arg(name));
        connect(front, &QAction::triggered, this, [this, id] {
            m_dashboard.bringToFront(id);
            Q_EMIT dashboardEdited();
            update();
        });

        QAction* back = menu.addAction(tr("Send '%1' to Back").arg(name));
        connect(back, &QAction::triggered, this, [this, id] {
            m_dashboard.sendToBack(id);
            Q_EMIT dashboardEdited();
            update();
        });

        QAction* remove = menu.addAction(tr("Delete '%1'").arg(name));
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

    if (m_editing && !m_selected.empty() && event->key() == Qt::Key_D
        && event->modifiers().testFlag(Qt::ControlModifier)) {
        (void)duplicateSelected();
        return;
    }

    QWidget::keyPressEvent(event);
}

bool DashboardPanel::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = detailsAt(help->pos());

        if (text.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }

        return true;
    }

    return QWidget::event(event);
}

// ---------------------------------------------------------------------------
// Drag and drop
// ---------------------------------------------------------------------------

QStringList DashboardPanel::pictureFilesIn(const QMimeData* mime)
{
    QStringList files;

    if (mime == nullptr || !mime->hasUrls()) {
        return files;
    }

    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }

        const QString path = url.toLocalFile();

        if (isImagePath(path.toStdString())) {
            files << path;
        }
    }

    return files;
}

bool DashboardPanel::canAccept(const QMimeData* mime) const
{
    // Only while editing: in Run mode a drop would add a widget to a dashboard that nobody can see
    // being edited.
    return m_editing && mime != nullptr
           && (hasSignalReferences(mime) || !pictureFilesIn(mime).isEmpty());
}

void DashboardPanel::dragEnterEvent(QDragEnterEvent* event)
{
    if (!canAccept(event->mimeData())) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
}

void DashboardPanel::dragMoveEvent(QDragMoveEvent* event)
{
    if (!canAccept(event->mimeData())) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();

    // The widget a drop would land on: a single signal onto a widget that reads one, a picture onto
    // one that has a picture of its own. Anywhere else a drop makes a new widget, and there is
    // nothing to highlight.
    std::string target;

    if (const DashboardWidget* widget = widgetAt(event->position().toPoint()); widget != nullptr) {
        const QList<SignalReference> references = signalReferencesOf(event->mimeData());
        const bool oneSignal = references.size() == 1 && !references.front().signal.isEmpty();

        if ((oneSignal && takesASignal(*widget))
            || (!oneSignal && traitsOf(widget->kind).image
                && !pictureFilesIn(event->mimeData()).isEmpty())) {
            target = widget->id;
        }
    }

    if (target != m_dropTarget) {
        m_dropTarget = target;
        update();
    }
}

void DashboardPanel::dragLeaveEvent(QDragLeaveEvent* event)
{
    Q_UNUSED(event)

    if (!m_dropTarget.empty()) {
        m_dropTarget.clear();
        update();
    }
}

void DashboardPanel::dropEvent(QDropEvent* event)
{
    const QMimeData* mime = event->mimeData();

    if (!canAccept(mime)) {
        event->ignore();
        return;
    }

    const QPoint where = event->position().toPoint();

    m_dropTarget.clear();

    if (hasSignalReferences(mime)) {
        std::vector<DictionaryEntry> entries;
        std::vector<std::string> messages;
        QStringList unknown;

        for (const SignalReference& reference : signalReferencesOf(mime)) {
            const std::string message = reference.message.toStdString();
            const std::string signal = reference.signal.toStdString();

            if (signal.empty()) {
                if (m_dictionary.hasMessage(message)) {
                    messages.push_back(message);
                } else {
                    unknown << reference.message;
                }
            } else if (const DictionaryEntry* entry = m_dictionary.find(message, signal);
                       entry != nullptr) {
                entries.push_back(*entry);
            } else {
                unknown << reference.message + QLatin1Char('.') + reference.signal;
            }
        }

        const DashboardWidget* target = widgetAt(where);

        if (entries.size() == 1 && messages.empty() && target != nullptr && takesASignal(*target)) {
            // One signal onto a widget that reads one: that widget now reads it.
            (void)bindWidget(target->id, entries.front());
        } else {
            QPoint cursor = where;

            for (const std::string& message : messages) {
                const std::string id = addTable(message, cursor);

                if (const DashboardWidget* table = m_dashboard.find(id); table != nullptr) {
                    cursor.ry() += static_cast<int>(table->height) + kGridGap;
                }
            }

            (void)addSignals(entries, std::nullopt, cursor);
        }

        if (!unknown.isEmpty()) {
            Q_EMIT reported(
                tr("Not in any loaded database: %1. Import its .dbc in the DBC Explorer first.")
                    .arg(unknown.join(QStringLiteral(", "))),
                true);
        }
    } else {
        const QStringList files = pictureFilesIn(mime);
        const DashboardWidget* target = widgetAt(where);
        QPoint cursor = where;

        for (qsizetype i = 0; i < files.size(); ++i) {
            // A picture onto a widget that has one of its own is that widget's new picture; more
            // than one file, or anywhere else, makes Images.
            if (i == 0 && files.size() == 1 && target != nullptr && traitsOf(target->kind).image) {
                (void)setPicture(target->id, files.at(i));
                continue;
            }

            const std::string id = addImage(files.at(i), cursor);

            if (const DashboardWidget* image = m_dashboard.find(id); image != nullptr) {
                cursor.rx() += static_cast<int>(image->width) + kGridGap;
            }
        }
    }

    event->acceptProposedAction();
}

// ---------------------------------------------------------------------------
// Making widgets
// ---------------------------------------------------------------------------

std::string DashboardPanel::place(DashboardWidget widget)
{
    widget.id =
        m_dashboard.uniqueId(widget.id.empty() ? std::string{nameOf(widget.kind)} : widget.id);

    const std::string id = widget.id;
    const bool behind = widget.kind == DashboardWidgetKind::Group;

    m_dashboard.add(std::move(widget));

    // A group goes behind what it groups, and what is put on it afterwards goes in front.
    if (behind) {
        m_dashboard.sendToBack(id);
    }

    m_selected = id;

    Q_EMIT selectionChanged(QString::fromStdString(m_selected));
    Q_EMIT dashboardEdited();

    syncClusters();
    update();

    return id;
}

void DashboardPanel::addWidget(DashboardWidgetKind kind, const QPoint& where)
{
    DashboardWidget widget = defaultWidgetOf(kind);
    widget.x = where.x();
    widget.y = where.y();

    // Unbound, and validate() says so: a control needs a variable to write and a reader needs
    // something to read, and neither is guessed.
    (void)place(std::move(widget));
}

std::vector<std::string> DashboardPanel::addSignals(const std::vector<DictionaryEntry>& entries,
                                                    std::optional<DashboardWidgetKind> kind,
                                                    const QPoint& where)
{
    std::vector<std::string> ids;

    // Left to right, then down: wrapped at the edge of what is on screen, or after a few when the
    // panel has not been shown yet.
    const int right = std::max(width(), where.x() + 3 * 220);

    QPoint cursor = where;
    int rowHeight = 0;

    for (const DictionaryEntry& entry : entries) {
        DashboardWidget widget = suggestWidget(entry, kind);

        if (cursor.x() > where.x() && cursor.x() + static_cast<int>(widget.width) > right) {
            cursor.setX(where.x());
            cursor.ry() += rowHeight + kGridGap;
            rowHeight = 0;
        }

        widget.x = cursor.x();
        widget.y = cursor.y();

        cursor.rx() += static_cast<int>(widget.width) + kGridGap;
        rowHeight = std::max(rowHeight, static_cast<int>(widget.height));

        ids.push_back(place(std::move(widget)));
    }

    return ids;
}

std::string DashboardPanel::addTable(std::string_view message, const QPoint& where)
{
    const std::size_t count = m_dictionary.signalsOf(message).size();

    DashboardWidget table = suggestTable(message, count);
    table.x = where.x();
    table.y = where.y();

    return place(std::move(table));
}

std::string DashboardPanel::addImage(const QString& file, const QPoint& where)
{
    const std::string utf8 = file.toStdString();

    if (!isImagePath(utf8)) {
        Q_EMIT reported(tr("%1 is not a picture this build can show (PNG, JPEG, BMP, GIF or SVG).")
                            .arg(QDir::toNativeSeparators(file)),
                        true);
        return {};
    }

    DashboardWidget image = defaultWidgetOf(DashboardWidgetKind::Image);
    image.id = idFor(QFileInfo{file}.completeBaseName().toStdString());
    image.image = storableAsset(utf8, m_images.baseDirectory().toStdString());
    image.x = where.x();
    image.y = where.y();

    // Sized to the picture's own proportions, with its longer side at the default widget's.
    if (const QSize size = m_images.sizeOf(image.image); size.isValid() && !size.isEmpty()) {
        const double longest = std::max(defaultSizeOf(DashboardWidgetKind::Image).width,
                                        defaultSizeOf(DashboardWidgetKind::Image).height);
        const double scale = longest / std::max(size.width(), size.height());

        image.width = std::max(40.0, size.width() * scale);
        image.height = std::max(30.0, size.height() * scale);
    }

    return place(std::move(image));
}

bool DashboardPanel::bindWidget(const std::string& id, const DictionaryEntry& entry)
{
    auto* widget = const_cast<DashboardWidget*>(m_dashboard.find(id));

    if (widget == nullptr || !takesASignal(*widget)) {
        return false;
    }

    applyEntry(*widget, entry);

    // The form is showing this widget, and it just changed under it.
    m_selected = id;

    Q_EMIT selectionChanged(QString::fromStdString(id));
    Q_EMIT dashboardEdited();

    update();
    return true;
}

bool DashboardPanel::setPicture(const std::string& id, const QString& file)
{
    auto* widget = const_cast<DashboardWidget*>(m_dashboard.find(id));

    if (widget == nullptr || !traitsOf(widget->kind).image || !isImagePath(file.toStdString())) {
        return false;
    }

    widget->image = storableAsset(file.toStdString(), m_images.baseDirectory().toStdString());

    m_selected = id;

    Q_EMIT selectionChanged(QString::fromStdString(id));
    Q_EMIT dashboardEdited();

    update();
    return true;
}

std::string DashboardPanel::duplicateSelected()
{
    if (m_selected.empty()) {
        return {};
    }

    const std::string id = m_dashboard.duplicate(m_selected);

    if (id.empty()) {
        return {};
    }

    m_selected = id;

    Q_EMIT selectionChanged(QString::fromStdString(id));
    Q_EMIT dashboardEdited();

    syncClusters();
    update();

    return id;
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

void DashboardPanel::addFromPicker(const QPoint& where)
{
    if (m_dictionary.empty()) {
        Q_EMIT reported(tr("No database is loaded. Import a .dbc in the DBC Explorer, then add "
                           "signals from it - or drag them here from there."),
                        false);
        return;
    }

    SignalPickerDialog dialog{m_dictionary, SignalPickerDialog::Mode::AddWidgets, this};

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QPoint cursor = where;

    for (const std::string& message : dialog.chosenMessages()) {
        const std::string id = addTable(message, cursor);

        if (const DashboardWidget* table = m_dashboard.find(id); table != nullptr) {
            cursor.ry() += static_cast<int>(table->height) + kGridGap;
        }
    }

    (void)addSignals(dialog.chosenSignals(), dialog.chosenKind(), cursor);
}

void DashboardPanel::addImageFromDialog(const QPoint& where)
{
    QStringList filters;
    for (const std::string_view extension : imageExtensions()) {
        filters << QStringLiteral("*.")
                       + QString::fromLatin1(extension.data(),
                                             static_cast<qsizetype>(extension.size()));
    }

    const QString file = QFileDialog::getOpenFileName(
        this,
        tr("Choose a picture"),
        m_images.baseDirectory(),
        tr("Pictures (%1);;All files (*)").arg(filters.join(QLatin1Char(' '))));

    if (!file.isEmpty()) {
        (void)addImage(file, where);
    }
}

// ---------------------------------------------------------------------------
// Asking
// ---------------------------------------------------------------------------

std::vector<TableRow> DashboardPanel::tableRowsOf(const std::string& widgetId) const
{
    std::vector<TableRow> rows;

    const DashboardWidget* widget = m_dashboard.find(widgetId);

    if (widget == nullptr || widget->binding.source != DashboardBinding::Source::Message) {
        return rows;
    }

    for (const DictionaryEntry* entry : m_dictionary.signalsOf(widget->binding.message)) {
        DashboardBinding binding;
        binding.source = DashboardBinding::Source::Signal;
        binding.message = entry->message;
        binding.signal = entry->signal;

        const Reading reading = readBinding(binding);

        TableRow row;
        row.name = QString::fromStdString(prettyName(entry->signal));
        row.known = reading.known;
        row.value = QStringLiteral("--");

        if (reading.known) {
            // The word the database has for the value, if it has one; otherwise the number, to as
            // many places as the signal's resolution deserves.
            const SignalValueName* named = nullptr;

            for (const SignalValueName& value : entry->values) {
                if (std::abs(entry->physical(value.value) - reading.value) <= 1e-9) {
                    named = &value;
                    break;
                }
            }

            if (named != nullptr) {
                row.value = QString::fromStdString(named->name);
                row.tone = inferTone(named->name);
            } else {
                row.value = QString::number(reading.value, 'f', decimalsFor(entry->factor));

                if (!entry->unit.empty()) {
                    row.value += QLatin1Char(' ') + QString::fromStdString(entry->unit);
                }
            }
        }

        rows.push_back(std::move(row));
    }

    return rows;
}

void DashboardPanel::syncFaults() const
{
    if (m_j1939 == nullptr) {
        // A graph with no J1939 block, or a panel not yet given the network: an empty list.
        if (!m_faultsFresh) {
            m_faults.clear();
            m_faultRows.clear();
            m_faultsRevision = 0;
            m_faultsFresh = true;
        }

        return;
    }

    // The cheap question first: an atomic load. Copying what the block published to find out
    // whether it differs would be the expensive half of the work to answer the cheap question.
    const std::uint64_t revision = m_j1939->revision();

    if (m_faultsFresh && revision == m_faultsRevision) {
        return;
    }

    const J1939NetworkSnapshot snapshot = m_j1939->snapshot();

    // The revision the copy reports, which is exactly the publication it is from: the network reads
    // both under the one lock. A publication that lands after it is then a change the next tick
    // sees.
    m_faultsRevision = snapshot.revision;
    m_faultsFresh = true;

    m_faults = dashboardFaultsOf(snapshot, m_j1939Names);
    m_faultRows.clear();
    m_faultRows.reserve(m_faults.size());

    for (const DashboardFault& fault : m_faults) {
        FaultRow row;
        row.code = faultCodeText(fault.code);
        row.meaning = faultMeaningText(fault.code);
        row.count = faultCountText(fault.code);
        row.ecu = faultEcuText(fault);
        row.tone = fault.tone;

        m_faultRows.push_back(std::move(row));
    }
}

std::vector<FaultRow> DashboardPanel::faultRows() const
{
    syncFaults();

    return m_faultRows;
}

FaultLamps DashboardPanel::faultLamps() const
{
    FaultLamps lamps;

    if (m_variables == nullptr) {
        return lamps;
    }

    // The same variables the cluster reads. A variable nobody has written reads as zero, which is a
    // dark lamp, so a bus with no J1939 block lights none.
    const auto lit = [this](std::string_view name) {
        return m_variables->value(std::string{name}) >= 0.5;
    };

    lamps.stop = lit(kFaultLampVariables[0]);
    lamps.warning = lit(kFaultLampVariables[1]);
    lamps.malfunction = lit(kFaultLampVariables[2]);
    lamps.protect = lit(kFaultLampVariables[3]);

    return lamps;
}

std::uint64_t DashboardPanel::faultsToken() const
{
    syncFaults();

    const FaultLamps lamps = faultLamps();

    std::uint64_t token = m_faultsRevision;
    token = mix(token, lamps.stop ? 1U : 0U);
    token = mix(token, lamps.warning ? 1U : 0U);
    token = mix(token, lamps.malfunction ? 1U : 0U);
    token = mix(token, lamps.protect ? 1U : 0U);

    return token;
}

QStringList DashboardPanel::faultDetails() const
{
    syncFaults();

    QStringList lines;
    const FaultLamps lamps = faultLamps();

    QStringList lit;
    if (lamps.stop) {
        lit << tr("stop");
    }
    if (lamps.warning) {
        lit << tr("warning");
    }
    if (lamps.malfunction) {
        lit << tr("malfunction indicator");
    }
    if (lamps.protect) {
        lit << tr("protect");
    }

    lines << (lit.isEmpty() ? tr("No lamp is lit")
                            : tr("Lamps lit: %1").arg(lit.join(QStringLiteral(", "))));

    if (m_faults.empty()) {
        lines << tr("No active faults");
        return lines;
    }

    // A tooltip is not a report: the first few in full, and a count of the rest.
    constexpr qsizetype kMostInFull = 6;

    for (qsizetype i = 0; i < std::min<qsizetype>(kMostInFull, std::ssize(m_faults)); ++i) {
        lines << QString{};
        lines << faultDetailsText(m_faults[static_cast<std::size_t>(i)]);
    }

    if (std::ssize(m_faults) > kMostInFull) {
        lines << QString{};
        lines << tr("+ %n more", nullptr, static_cast<int>(std::ssize(m_faults) - kMostInFull));
    }

    return lines;
}

std::uint64_t DashboardPanel::trendNewestNs(const DashboardWidget& widget) const
{
    if (widget.binding.source == DashboardBinding::Source::Variable) {
        return m_trailsNowNs;
    }

    return m_plots != nullptr ? m_plots->newestTimestampNs() : 0;
}

void DashboardPanel::recordTrails(Clock::time_point now)
{
    m_trailsNowNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

    std::unordered_set<std::string> kept;

    for (const DashboardWidget& widget : m_dashboard.widgets()) {
        if (widget.kind != DashboardWidgetKind::Trend
            || widget.binding.source != DashboardBinding::Source::Variable) {
            continue;
        }

        kept.insert(widget.id);

        const Reading reading = readBinding(widget.binding);
        std::deque<SignalSample>& trail = m_trails[widget.id];

        if (reading.known) {
            trail.push_back(SignalSample{m_trailsNowNs, reading.value});
        }

        const auto windowNs = static_cast<std::uint64_t>(widget.windowSeconds * 1e9);

        while (!trail.empty() && m_trailsNowNs - trail.front().timestampNs > windowNs) {
            trail.pop_front();
        }
    }

    // A Trend that was deleted, or bound to something else, leaves no trail behind.
    std::erase_if(m_trails, [&kept](const auto& item) { return !kept.contains(item.first); });
}

const SeriesWindow* DashboardPanel::trendWindowOf(const DashboardWidget& widget) const
{
    if (widget.binding.source == DashboardBinding::Source::Variable) {
        const auto trail = m_trails.find(widget.id);

        if (trail == m_trails.end() || trail->second.empty()) {
            return nullptr;
        }

        m_windows.assign(1, SeriesWindow{});
        m_windows.front().samples.assign(trail->second.begin(), trail->second.end());
        return &m_windows.front();
    }

    if (m_plots == nullptr || widget.binding.source != DashboardBinding::Source::Signal) {
        return nullptr;
    }

    const SeriesId id = m_plots->find(widget.binding.message + "." + widget.binding.signal);

    if (id == kNoSeries) {
        return nullptr;
    }

    const std::uint64_t newest = m_plots->newestTimestampNs();
    const auto windowNs = static_cast<std::uint64_t>(widget.windowSeconds * 1e9);
    const std::uint64_t since = newest > windowNs ? newest - windowNs : 0;

    // More than the line has pixels for, and the newest kept if there are more than that.
    constexpr std::size_t kMostSamples = 1500;

    const std::array<SeriesId, 1> ids{id};
    m_plots->readWindows(ids, since, kMostSamples, m_windows);

    return m_windows.empty() ? nullptr : &m_windows.front();
}

QString DashboardPanel::detailsAt(const QPoint& point) const
{
    const DashboardWidget* widget = widgetAt(point);

    if (widget == nullptr) {
        return {};
    }

    QStringList lines;
    lines << QStringLiteral("%1  ·  %2")
                 .arg(displayNameOf(widget->kind), QString::fromStdString(widget->id));

    const DashboardBinding& binding = widget->binding;

    switch (binding.source) {
    case DashboardBinding::Source::Signal:
        lines << QString::fromStdString(binding.message + "." + binding.signal);
        break;
    case DashboardBinding::Source::Variable:
        lines << tr("variable %1").arg(QString::fromStdString(binding.variable));
        break;
    case DashboardBinding::Source::Message:
        lines << tr("every signal of %1").arg(QString::fromStdString(binding.message));
        break;
    case DashboardBinding::Source::None:
        if (traitsOf(widget->kind).binding == BindingNeed::Required) {
            lines << tr("(not bound)");
        }
        break;
    }

    // A fault list has no value of its own to read; it says what the bus is reporting.
    if (widget->kind == DashboardWidgetKind::Faults) {
        lines << faultDetails();
    }

    // What it reads now: only for the kinds with a value of their own.
    if (binding.source == DashboardBinding::Source::Signal
        || binding.source == DashboardBinding::Source::Variable) {
        const Reading reading = read(*widget);

        if (reading.stale) {
            lines << tr("No new sample for more than %1 ms").arg(widget->staleMs);
        } else if (reading.known) {
            lines << tr("Reads %1").arg(formatReading(*widget, reading));
        } else {
            lines << tr("No value yet");
        }
    }

    if (binding.source == DashboardBinding::Source::Signal) {
        if (const DictionaryEntry* entry = m_dictionary.find(binding.message, binding.signal);
            entry != nullptr) {
            lines << summaryOf(*entry);

            if (!entry->comment.empty()) {
                lines << QString::fromStdString(entry->comment);
            }
        }
    }

    if (const QString finding = findingFor(widget->id); !finding.isEmpty()) {
        lines << finding;
    }

    if (!widget->image.empty()) {
        if (const QString problem = m_images.problemWith(widget->image); !problem.isEmpty()) {
            lines << problem;
        }
    }

    return lines.join(QLatin1Char('\n'));
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
    case DashboardBinding::Source::Message:
        return QString::fromStdString(widget.binding.message);
    case DashboardBinding::Source::None:
        break;
    }

    // Unbound, and in edit mode that is the thing worth saying. An Image, a Group and a Label have
    // nothing to be bound to, and say nothing.
    if (traitsOf(widget.kind).binding == BindingNeed::Required
        || traitsOf(widget.kind).binding == BindingNeed::Message) {
        return tr("(not bound)");
    }

    return {};
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
                         m_editing ? tr("Right-click to add a widget, or drag signals here\n"
                                        "from the DBC Explorer.")
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

    const PaintContext context{
        theme, widget, rect, reading, captionOf(widget), m_images, m_editing};

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
    case DashboardWidgetKind::Bar:
        paintBar(painter, context);
        break;
    case DashboardWidgetKind::State:
        paintState(painter, context);
        break;
    case DashboardWidgetKind::Image:
        paintImage(painter, context);
        break;
    case DashboardWidgetKind::Trend:
        paintTrend(painter, context, trendWindowOf(widget), trendNewestNs(widget));
        break;
    case DashboardWidgetKind::Table: {
        QString hint;

        if (widget.binding.message.empty()) {
            hint = tr("Pick the message this table lists.");
        } else if (!m_dictionary.hasMessage(widget.binding.message)) {
            hint = tr("No loaded database describes %1.\nImport it in the DBC Explorer.")
                       .arg(QString::fromStdString(widget.binding.message));
        } else {
            hint = tr("%1 has no signals.").arg(QString::fromStdString(widget.binding.message));
        }

        paintTable(painter, context, tableRowsOf(widget.id), hint);
        break;
    }
    case DashboardWidgetKind::Group:
        paintGroup(painter, context);
        break;
    case DashboardWidgetKind::Faults:
        syncFaults();
        paintFaults(painter, context, faultLamps(), m_faultRows);
        break;
    }

    if (reading.stale) {
        paintStaleBadge(painter, context);
    }

    painter.restore();

    if (!m_editing) {
        return;
    }

    // --- Edit chrome -------------------------------------------------------
    const bool selected = widget.id == m_selected;
    const bool dropTarget = widget.id == m_dropTarget;

    QPen outline{selected || dropTarget ? theme.accent : theme.border};
    outline.setWidthF(selected || dropTarget ? 2.0 : 1.0);
    outline.setStyle(selected || dropTarget ? Qt::SolidLine : Qt::DashLine);

    painter.setPen(outline);
    painter.setBrush(dropTarget ? QBrush{withAlpha(theme.accent, 40)} : QBrush{Qt::NoBrush});
    painter.drawRect(rect);

    if (selected) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.accent);
        painter.drawRect(handleOf(widget));
    }

    // A mark on a widget whose message or signal no loaded database has. Hover says which, and what
    // is near it.
    if (m_findings.contains(widget.id)) {
        const QRectF mark{rect.left() + 3.0, rect.top() + 3.0, 16.0, 16.0};

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.lampAmber);
        painter.drawEllipse(mark);

        QFont font = painter.font();
        font.setBold(true);
        font.setPointSizeF(9.0);
        painter.setFont(font);
        painter.setPen(theme.textInverted);
        painter.drawText(mark, Qt::AlignCenter, QStringLiteral("!"));
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

    // Past a limit the arc and the number take its colour; between them the gauge is what it was.
    const DashboardTone tone =
        reading.known ? widget.limits.toneOf(reading.value) : DashboardTone::Neutral;

    if (reading.known) {
        const double swept = fractionOf(reading.value, widget.minimum, widget.maximum);

        QPen live{tone == DashboardTone::Neutral ? theme.accent : toneColor(theme, tone)};
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

    // Where the limits are on the scale: a short mark across the track at the angle each is.
    const auto mark = [&](const std::optional<double>& limit, DashboardTone meaning) {
        if (!limit.has_value() || !(*limit > widget.minimum) || !(*limit < widget.maximum)) {
            return;
        }

        const double at = fractionOf(*limit, widget.minimum, widget.maximum);
        const double radians = (kGaugeStartDegrees + kGaugeSweepDegrees * at) * M_PI / 180.0;
        const double outer = face.width() / 2.0 + track.widthF() * 0.8;
        const double inner = face.width() / 2.0 - track.widthF() * 0.8;
        const QPointF centre = face.center();

        painter.setPen(QPen{toneColor(theme, meaning), 2.0, Qt::SolidLine, Qt::FlatCap});
        painter.drawLine(
            QPointF{centre.x() + std::cos(radians) * inner, centre.y() - std::sin(radians) * inner},
            QPointF{centre.x() + std::cos(radians) * outer,
                    centre.y() - std::sin(radians) * outer});
    };

    mark(widget.limits.warningBelow, DashboardTone::Warning);
    mark(widget.limits.warningAbove, DashboardTone::Warning);
    mark(widget.limits.criticalBelow, DashboardTone::Danger);
    mark(widget.limits.criticalAbove, DashboardTone::Danger);

    // The number under the needle: a gauge is read for the shape of the change
    // and the number is read for the value, and both are wanted at once.
    painter.setPen(reading.known
                       ? (tone == DashboardTone::Neutral ? theme.text : toneColor(theme, tone))
                       : theme.textMuted);

    const QString value = reading.known
                              ? QStringLiteral("%1").arg(reading.value, 0, 'f', widget.decimals)
                              : QStringLiteral("--");

    const QString unit = QString::fromStdString(widget.unit);

    painter.drawText(
        QRectF{rect.left(),
               rect.center().y() + rect.height() * 0.18,
               rect.width(),
               rect.height() * 0.2},
        Qt::AlignCenter,
        fitted(painter, unit.isEmpty() ? value : value + QStringLiteral(" ") + unit, rect.width()));

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{rect.left(), rect.bottom() - rect.height() * 0.2, rect.width(), rect.height() * 0.2},
        Qt::AlignCenter,
        fitted(painter, captionOf(widget), rect.width()));
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

    // The name of the state when the widget has been given names for its values, and the number
    // with its unit when it has not or the value is one it has no name for.
    const QString text = formatReading(widget, reading);

    const DashboardState* state = reading.known ? widget.stateOf(reading.value) : nullptr;

    DashboardTone tone = state != nullptr ? state->tone : DashboardTone::Neutral;

    if (tone == DashboardTone::Neutral && reading.known) {
        tone = widget.limits.toneOf(reading.value);
    }

    QFont big = painter.font();

    // Scaled to the box rather than fixed: the whole point of a numeric readout
    // is that it can be read from across a workshop, which means it has to grow
    // when somebody makes the box bigger.
    big.setBold(true);

    // Smaller before it is cut: "356 kPa" in a box that has room for "52 kPa" is a reading somebody
    // needs to be able to read, and an ellipsis where its unit was is not that. Cut only when it
    // cannot be made to fit.
    const double room = rect.width() - 8.0;

    for (double points = std::max(10.0, rect.height() * 0.35); points > 6.0; points *= 0.92) {
        big.setPointSizeF(points);

        if (QFontMetricsF{big}.horizontalAdvance(text) <= room) {
            break;
        }
    }

    painter.setFont(big);

    painter.setPen(reading.known
                       ? (tone == DashboardTone::Neutral ? theme.text : toneColor(theme, tone))
                       : theme.textMuted);
    painter.drawText(
        QRectF{rect.left(), rect.top() + rect.height() * 0.1, rect.width(), rect.height() * 0.55},
        Qt::AlignCenter,
        painter.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(room)));

    QFont small = painter.font();
    small.setPointSizeF(std::max(8.0, rect.height() * 0.16));
    small.setBold(false);
    painter.setFont(small);

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.3, rect.width(), rect.height() * 0.25},
        Qt::AlignCenter,
        fitted(painter, captionOf(widget), rect.width()));
}

void DashboardPanel::paintLamp(QPainter& painter,
                               const DashboardWidget& widget,
                               const QRectF& rect,
                               const Reading& reading)
{
    const Theme theme = currentTheme();

    const bool on = reading.known && reading.value >= widget.threshold;

    const DashboardState* state = reading.known ? widget.stateOf(reading.value) : nullptr;

    // Its own picture for the state, if the widget has been given one; otherwise the widget's.
    const std::string& picture =
        state != nullptr && !state->image.empty() ? state->image : widget.image;

    const double diameter = std::min(rect.width(), rect.height() * 0.6) * 0.7;
    QRectF bulb{0.0, 0.0, diameter, diameter};
    bulb.moveCenter(QPointF{rect.center().x(), rect.top() + rect.height() * 0.4});

    bool drewPicture = false;

    if (!picture.empty()) {
        // A picture in place of the bulb: lit it is as it is, and dark it is the same picture
        // turned down, so that it still looks like the lamp that was on.
        drewPicture = m_images.draw(
            painter,
            bulb.adjusted(-diameter * 0.15, -diameter * 0.15, diameter * 0.15, diameter * 0.15),
            picture,
            on ? 1.0 : 0.25);
    }

    if (!drewPicture) {
        // What the state means colours the bulb; a lamp with no opinion about it is the amber it
        // always was.
        QColor lit = theme.warning;

        if (state != nullptr && state->tone != DashboardTone::Neutral
            && state->tone != DashboardTone::Off) {
            lit = toneColor(theme, state->tone);
        }

        // Off is a dim outline rather than a different colour: a lamp that is off
        // still has to look like the lamp that was on, or nobody believes it is the
        // same one.
        QColor fill = lit;
        if (!on) {
            fill.setAlpha(40);
        }

        painter.setBrush(fill);
        painter.setPen(QPen{on ? lit : theme.border, 2.0});
        painter.drawEllipse(bulb);
    }

    // The name of the state it is in, with the title: "Door - Open".
    QString caption = captionOf(widget);

    if (state != nullptr && !state->label.empty()) {
        caption += QStringLiteral(" – ") + QString::fromStdString(state->label);
    }

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.3, rect.width(), rect.height() * 0.25},
        Qt::AlignCenter,
        painter.fontMetrics().elidedText(
            caption, Qt::ElideRight, static_cast<int>(rect.width() - 6.0)));
}

void DashboardPanel::paintSlider(QPainter& painter,
                                 const DashboardWidget& widget,
                                 const QRectF& rect,
                                 const Reading& reading)
{
    const Theme theme = currentTheme();

    const bool horizontal = rect.width() >= rect.height();
    const double swept = fractionOf(reading.value, widget.minimum, widget.maximum);

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
        fitted(painter,
               QStringLiteral("%1  %2")
                   .arg(captionOf(widget))
                   .arg(reading.value, 0, 'f', widget.decimals),
               rect.width()));
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

    const double swept = fractionOf(reading.value, widget.minimum, widget.maximum);
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
        fitted(painter,
               QStringLiteral("%1  %2")
                   .arg(captionOf(widget))
                   .arg(reading.value, 0, 'f', widget.decimals),
               rect.width()));
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
                     widget.kind == DashboardWidgetKind::Switch
                         ? (on ? tr("ON") : tr("OFF"))
                         : fitted(painter, captionOf(widget), face.width()));

    painter.setPen(theme.textMuted);
    painter.drawText(
        QRectF{
            rect.left(), rect.bottom() - rect.height() * 0.28, rect.width(), rect.height() * 0.26},
        Qt::AlignCenter,
        fitted(painter, captionOf(widget), rect.width()));
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
