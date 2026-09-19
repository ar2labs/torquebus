// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/canvas/CanvasPanel.h"

#include "ui/canvas/PipelineGraphModel.h"
#include "ui/theme/ThemeManager.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsView>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPalette>
#include <QSplitter>
#include <QString>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtGlobal>

namespace torquebus::ui {
namespace {

/// The role holding a palette entry's catalog type name.
constexpr int kTypeNameRole = Qt::UserRole + 1;

/// QtNodes reads its styles from JSON, so the theme has to be rendered into it.
///
/// Three separate singletons - view, node and connection - each with its own
/// setter, and all three are process-wide rather than per-view. That is a
/// limitation worth knowing before someone opens a second canvas expecting it
/// to have its own colours.
[[nodiscard]] QString styleJson(const Theme& theme)
{
    const auto rgb = [](const QColor& colour) {
        return QJsonArray{colour.red(), colour.green(), colour.blue()};
    };

    // Dedicated theme roles, not surface roles borrowed from the panel ladder.
    //
    // The first version used `separator`, `background` and `tabStrip`. Those
    // are chosen to sit 6-10 points apart so a panel reads as a different
    // surface from the window behind it - which is exactly the wrong magnitude
    // for a line drawn every 15 pixels, and it came out as a hard mesh. Worse,
    // in the dark theme `tabStrip` is *darker* than `background`, so the 150 px
    // guide was fainter than the 15 px filler it exists to organise.
    QJsonObject view;
    view["BackgroundColor"] = rgb(theme.canvas);
    view["FineGridColor"] = rgb(theme.canvasGridFine);
    view["CoarseGridColor"] = rgb(theme.canvasGridCoarse);

    // A node is a panel: same surface, same border, same accent when selected.
    // The gradient is flat on purpose - four stops of the same colour - because
    // a shaded block would be the one thing in this window with a gloss on it.
    QJsonObject node;
    node["NormalBoundaryColor"] = rgb(theme.border);
    node["SelectedBoundaryColor"] = rgb(theme.accent);
    node["GradientColor0"] = rgb(theme.panel);
    node["GradientColor1"] = rgb(theme.panel);
    node["GradientColor2"] = rgb(theme.panel);
    node["GradientColor3"] = rgb(theme.panel);
    node["ShadowColor"] = rgb(theme.separator);
    node["ShadowEnabled"] = false;
    node["FontColor"] = rgb(theme.text);
    node["FontColorFaded"] = rgb(theme.textMuted);
    node["ConnectionPointColor"] = rgb(theme.textMuted);
    node["FilledConnectionPointColor"] = rgb(theme.accent);
    node["ErrorColor"] = rgb(theme.error);
    node["WarningColor"] = rgb(theme.warning);
    node["ToolTipIconColor"] = rgb(theme.text);
    node["PenWidth"] = 1.0;
    node["HoveredPenWidth"] = 1.5;
    node["ConnectionPointDiameter"] = 8.0;
    node["Opacity"] = 1.0; // Opaque: a translucent block over a grid is unreadable.

    QJsonObject connection;
    connection["ConstructionColor"] = rgb(theme.textMuted);
    connection["NormalColor"] = rgb(theme.accent);
    connection["SelectedColor"] = rgb(theme.accentHover);
    connection["SelectedHaloColor"] = rgb(theme.accent);
    connection["HoveredColor"] = rgb(theme.accentHover);
    connection["LineWidth"] = 2.0;
    connection["ConstructionLineWidth"] = 2.0;
    connection["PointDiameter"] = 8.0;
    connection["UseDataDefinedColors"] = false;

    QJsonObject root;
    root["GraphicsViewStyle"] = view;
    root["NodeStyle"] = node;
    root["ConnectionStyle"] = connection;

    return QString::fromUtf8(QJsonDocument{root}.toJson(QJsonDocument::Compact));
}

} // namespace

CanvasPanel::CanvasPanel(GraphDescription& description, const NodeCatalog& catalog, QWidget* parent)
    : QWidget{parent}
    , m_description{description}
    , m_catalog{catalog}
{
    m_model = std::make_unique<PipelineGraphModel>(m_description, m_catalog);

    // Before the view exists, and that ordering is load-bearing.
    //
    // QtNodes::GraphicsView reads BackgroundColor exactly once, in its
    // constructor, and calls setBackgroundBrush with it (GraphicsView.cpp:44).
    // Everything else about the style is read at paint time. So a view built
    // before the style is set keeps QtNodes' default #353535 for the rest of
    // its life while its grid lines follow the theme - a dark grey canvas with
    // light-theme grid lines on it, which is what shipped.
    //
    // applyTheme() below sets the brush again for the same reason. Both are
    // needed: this one so the first paint is right, that one so a theme switch
    // is.
    if (ThemeManager* themes = ThemeManager::instance()) {
        applyStyles(themes->theme());
    }

    m_scene = new QtNodes::BasicGraphicsScene{*m_model, this};
    m_view = new QtNodes::GraphicsView{m_scene};

    m_palette = new QTreeWidget;
    m_palette->setHeaderHidden(true);
    m_palette->setRootIsDecorated(true);
    m_palette->setSelectionMode(QAbstractItemView::SingleSelection);
    m_palette->setMinimumWidth(150);
    m_palette->setMaximumWidth(260);

    buildPalette();

    m_splitter = new QSplitter{Qt::Horizontal, this};
    m_splitter->setObjectName(QStringLiteral("torquebus.splitter.canvas"));
    m_splitter->addWidget(m_palette);
    m_splitter->addWidget(m_view);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setCollapsible(0, true);
    m_splitter->setCollapsible(1, false);

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_splitter);

    // Double-click rather than drag and drop, for now. Dropping onto a
    // QGraphicsView means mapping the drop point into scene coordinates and
    // handling a drag that starts in one widget and ends in another; the double
    // click gets a node onto the canvas today, and the node is draggable the
    // moment it lands.
    connect(m_palette, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        addNodeFromPalette(item);
    });

    connect(m_model.get(), &PipelineGraphModel::nodeCreated, this, [this](QtNodes::NodeId) {
        Q_EMIT graphEdited();
    });
    connect(m_model.get(), &PipelineGraphModel::nodeDeleted, this, [this](QtNodes::NodeId) {
        Q_EMIT graphEdited();
    });
    connect(m_model.get(),
            &PipelineGraphModel::connectionCreated,
            this,
            [this](QtNodes::ConnectionId) { Q_EMIT graphEdited(); });
    connect(m_model.get(),
            &PipelineGraphModel::connectionDeleted,
            this,
            [this](QtNodes::ConnectionId) { Q_EMIT graphEdited(); });

    // Selection reaches the Properties panel by node id, not by pointer: the
    // canvas can be closed and reopened while a panel still shows a node.
    connect(
        m_scene, &QtNodes::BasicGraphicsScene::nodeSelected, this, [this](QtNodes::NodeId nodeId) {
            Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(nodeId)));
        });

    if (ThemeManager* themes = ThemeManager::instance()) {
        applyTheme(themes->theme());
        connect(themes, &ThemeManager::themeChanged, this, &CanvasPanel::applyTheme);
    }
}

CanvasPanel::~CanvasPanel()
{
    releaseGraph();
}

QByteArray CanvasPanel::splitterState() const
{
    return m_splitter != nullptr ? m_splitter->saveState() : QByteArray{};
}

void CanvasPanel::restoreSplitterState(const QByteArray& state)
{
    if (m_splitter == nullptr || state.isEmpty()) {
        return;
    }

    // A state that no longer fits - saved when the panel had a different number
    // of children - is refused by Qt. Said out loud rather than ignored: the
    // panel silently coming up in the default proportions after the user set
    // them looks like the setting was never saved.
    if (!m_splitter->restoreState(state)) {
        qWarning("TorqueBus: the saved Pipeline divider position could not be restored.");
    }
}

void CanvasPanel::releaseGraph()
{
    // Two lifetime problems, one function.
    //
    // First, inside this panel: BasicGraphicsScene holds
    // `AbstractGraphModel&` - our m_model - and the NodeGraphicsObjects it owns
    // reach into that model in twenty-odd places. Left to the default
    // destructor the order would be
    //
    //   1. ~CanvasPanel body       (nothing)
    //   2. members, reverse order  -> m_model DESTROYED here
    //   3. ~QWidget                -> deletes children, including the scene,
    //                                 which still references it
    //
    // Second, one level up: this panel is a child widget of a dock owned by the
    // main window, so it is destroyed inside ~QWidget of that window - which
    // runs *after* the window's own members, and m_description is one of them.
    // So even a correct destructor here would run too late.
    //
    // Hence a function the owner calls. Deleting the panel from the window's
    // destructor instead would work for the description but leave
    // KDDockWidgets holding a guest view onto a destroyed widget, trading one
    // dangling reference for another.
    //
    // Neither compiler nor test can see any of this, and a crash on exit that
    // happens on some runs is the worst kind to chase later.
    delete m_view;
    m_view = nullptr;

    delete m_scene;
    m_scene = nullptr;

    m_model.reset();
}

void CanvasPanel::buildPalette()
{
    m_palette->clear();

    // Grouped by the catalog's own category, in registration order, so the
    // palette reads Sources, Transforms, Simulation, Sinks - the order data
    // flows - rather than alphabetically.
    QMap<QString, QTreeWidgetItem*> groups;

    for (const NodeTypeInfo& info : m_catalog.types()) {
        const QString category = QString::fromStdString(info.category);

        QTreeWidgetItem* group = groups.value(category, nullptr);
        if (group == nullptr) {
            group = new QTreeWidgetItem{m_palette, QStringList{category}};
            group->setFlags(Qt::ItemIsEnabled); // A heading, not a choice.
            group->setExpanded(true);
            groups.insert(category, group);
        }

        auto* entry =
            new QTreeWidgetItem{group, QStringList{QString::fromStdString(info.displayName)}};
        entry->setData(0, kTypeNameRole, QString::fromStdString(info.typeName));
        entry->setToolTip(0, QString::fromStdString(info.description));
    }
}

void CanvasPanel::addNodeFromPalette(QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }

    if (!m_model || m_view == nullptr) {
        return;
    }

    const QString typeName = item->data(0, kTypeNameRole).toString();
    if (typeName.isEmpty()) {
        return; // A category heading.
    }

    const QtNodes::NodeId nodeId = m_model->addNode(typeName);
    if (nodeId == QtNodes::InvalidNodeId) {
        return;
    }

    // Placed at the centre of what the user is currently looking at, rather
    // than at the origin. A node dropped off-screen looks like nothing
    // happened, and the canvas scrolls.
    const QPointF centre = m_view->mapToScene(m_view->viewport()->rect().center());
    m_model->setNodeData(nodeId, QtNodes::NodeRole::Position, centre);

    // Selected, and announced. A block is dropped in order to be configured,
    // and most of them arrive incomplete on purpose - so the settings for the
    // one just added belong in front of the user, not one click away behind
    // whatever tab they were last on.
    if (QtNodes::NodeGraphicsObject* object = m_scene->nodeGraphicsObject(nodeId);
        object != nullptr) {
        m_scene->clearSelection();
        object->setSelected(true);
    }

    // Emitted by hand because QtNodes only raises nodeSelected from a mouse
    // press: selecting the item above tells the user's eye and nothing else.
    Q_EMIT nodeSelected(QString::fromStdString(m_model->descriptionId(nodeId)));
}

void CanvasPanel::reload()
{
    // Guarded because releaseGraph() may already have run: a queued signal
    // arriving during teardown is exactly the kind of thing that turns a clean
    // exit into an intermittent crash.
    if (m_model) {
        m_model->reload();
    }
}

void CanvasPanel::applyStyles(const Theme& theme)
{
    const QString json = styleJson(theme);

    // Process-wide singletons, not per-view - which is why this is static, and
    // why a second canvas would share these colours.
    QtNodes::GraphicsViewStyle::setStyle(json);
    QtNodes::NodeStyle::setNodeStyle(json);
    QtNodes::ConnectionStyle::setConnectionStyle(json);
}

void CanvasPanel::applyTheme(const Theme& theme)
{
    applyStyles(theme);

    // The one part of the style the view does not re-read. Setting the
    // singleton is not enough: the brush was copied out of it in the
    // constructor and nothing looks at it again.
    if (m_view != nullptr) {
        m_view->setBackgroundBrush(theme.canvas);
    }

    // The palette is a panel, so it is painted like one. Without this it takes
    // the view's colour from the enclosing dock and reads as part of the
    // canvas rather than as a list beside it.
    if (m_palette != nullptr) {
        QPalette palette = m_palette->palette();
        palette.setColor(QPalette::Base, theme.panel);
        palette.setColor(QPalette::Window, theme.panel);
        m_palette->setPalette(palette);
    }

    // The rest of the styles are read when items paint, and existing items do
    // not know they changed - so the scene has to be told to redraw.
    if (m_scene != nullptr) {
        m_scene->update();
    }
    if (m_view != nullptr) {
        m_view->update();
    }
}

} // namespace torquebus::ui
