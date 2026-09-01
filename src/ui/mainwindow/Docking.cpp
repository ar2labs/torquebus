// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/mainwindow/Docking.h"

#include <kddockwidgets/qtwidgets/ViewFactory.h>

#include <QIcon>
#include <QPalette>
#include <QVBoxLayout>
#include <QWidget>

/// Registers KDDockWidgets' own icon resource.
///
/// At true global scope, and not in an anonymous namespace either.
/// Q_INIT_RESOURCE expands to a block-scope `extern` declaration of the
/// generated initialiser; that declaration resolves against the enclosing
/// namespace, so putting this anywhere but the global namespace declares a
/// symbol that does not exist and fails to link. An unnamed namespace is still
/// a namespace, and would give the declaration internal linkage - the same
/// failure, with a more confusing message.
static void initializeDockingResources()
{
#ifdef KDDOCKWIDGETS_STATICLIB
    // KDDockWidgets keeps its icons - float, close, the drop indicators - in a
    // Qt resource compiled into the library. In a SHARED build that resource
    // registers itself when the DLL loads. In a STATIC build, which is what we
    // ask for, the registration lives in an object file nothing else
    // references, so the linker is free to drop it: the library links cleanly,
    // runs, and simply has no icons.
    //
    // That is why the float and close buttons on every panel were blank
    // rectangles. They were real buttons, correctly placed and clickable,
    // drawing an icon that had never been registered.
    Q_INIT_RESOURCE(kddockwidgets_resources);
#endif
}

namespace torquebus::ui {
namespace {

/// Supplies TorqueBus' own glyphs for the panel title-bar buttons.
///
/// KDDockWidgets' defaults are PNGs loaded from `:/img/close.png` and friends,
/// drawn as dark glyphs for a light title bar. Two things were wrong with
/// relying on them:
///
///   1. In a static build their resource is not registered unless something
///      forces it (see initializeDockingResources above), so the buttons were
///      blank rectangles - real, clickable, and invisible.
///   2. Even registered, a dark glyph on our dark chrome would be nearly as
///      invisible as no glyph at all.
///
/// Overriding iconForButtonType is the whole customisation: the buttons, their
/// placement and their behaviour stay KDDockWidgets'. Only the picture changes.
class DockButtonIconFactory final : public KDDockWidgets::QtWidgets::ViewFactory {
public:
    [[nodiscard]] QIcon iconForButtonType(KDDockWidgets::TitleBarButtonType type,
                                          qreal dpr) const override
    {
        Q_UNUSED(dpr) // SVG scales; there is no per-DPR asset to choose between.

        const QString name = resourceNameFor(type);
        if (name.isEmpty()) {
            return {};
        }

        return QIcon{QStringLiteral(":/icons/%1.svg").arg(name)};
    }

private:
    [[nodiscard]] static QString resourceNameFor(KDDockWidgets::TitleBarButtonType type)
    {
        using Type = KDDockWidgets::TitleBarButtonType;

        switch (type) {
        case Type::Close:      return QStringLiteral("panel-close");
        case Type::Minimize:   return QStringLiteral("panel-minimize");
        case Type::Maximize:   return QStringLiteral("panel-maximize");
        case Type::AutoHide:   return QStringLiteral("panel-autohide");
        case Type::UnautoHide: return QStringLiteral("panel-unautohide");

        // Float and Normal are the same affordance seen from either side -
        // "make this a window" and "put it back" - and KDDockWidgets ships one
        // icon for both. We follow suit rather than inventing a distinction the
        // framework does not make.
        case Type::Float:
        case Type::Normal:     return QStringLiteral("panel-float");

        case Type::AllTitleBarButtonTypes:
            break;
        }

        return {};
    }
};

} // namespace

void configureDockingSystem()
{
    initializeDockingResources();

    auto& config = KDDockWidgets::Config::self();

    // Ownership is taken by Config.
    config.setViewFactory(new DockButtonIconFactory);

    // By default KDDockWidgets' internal widgets reimplement paintEvent() and
    // draw their own frames, which is why several borders survived every rule
    // added to the style sheet: the style sheet was being painted over.
    //
    // Turning those paint events off makes each widget fall back to
    // QWidget::paintEvent, which honours the style sheet - the documented way
    // to style KDDockWidgets with CSS. From here the panel chrome is ours, and
    // a border that looks wrong is a rule in torquebus.qss rather than
    // something happening inside the library.
    // Built as an initialiser list rather than with `|`.
    //
    // KDDockWidgets declares Q_DECLARE_OPERATORS_FOR_FLAGS for Config::Flags
    // but not for Config::CustomizableWidgets, so `A | B` on these enumerators
    // has no QFlags overload to find and falls back to integral promotion -
    // producing an int that setDisabledPaintEvents() then refuses. QFlags'
    // initialiser-list constructor sidesteps the missing operator entirely.
    using CW = KDDockWidgets::Config::CustomizableWidget;
    config.setDisabledPaintEvents(KDDockWidgets::Config::CustomizableWidgets{
        CW::CustomizableWidget_TitleBar,
        CW::CustomizableWidget_TabBar,
        CW::CustomizableWidget_TabWidget,
        CW::CustomizableWidget_Frame,
        CW::CustomizableWidget_Separator,
        CW::CustomizableWidget_DockWidget,
    });

    // Behaviour chosen to match what engineers expect from CANoe / TSMaster:
    // panels can be torn off onto a second monitor, dropped as tabs, and the
    // central area always survives - closing the last analysis panel must not
    // leave an empty shell with no way back.
    auto flags = config.flags();
    flags |= KDDockWidgets::Config::Flag_AllowReorderTabs;
    flags |= KDDockWidgets::Config::Flag_AlwaysShowTabs;
    flags |= KDDockWidgets::Config::Flag_CloseOnlyCurrentTab;
    config.setFlags(flags);

    // Flag_ShowButtonsOnTabBarIfTitleBarHidden is deliberately NOT set.
    //
    // It puts float and close buttons at the right of the tab strip, and those
    // buttons have resisted three rounds of fixing: blank in both themes, with
    // a broken edge beside the second one. Two of those rounds were spent on
    // plausible causes that turned out not to be it.
    //
    // Shipping a control that looks broken is worse than not shipping it: every
    // panel is still closable from the View menu and still floats by dragging
    // its tab, so nothing is actually lost. describeDockChrome() exists to find
    // the real cause; when it is understood, this comes back.

    // The separator is the drag handle between panels, so its thickness is a
    // hit target before it is a visual choice. This was briefly set to 1px to
    // quieten a light band that looked like a border defect; at 1px the handle
    // became invisible AND unusable - trading a cosmetic problem for a
    // functional one, which is the wrong direction.
    //
    // The band was never too thick. It was the wrong colour: a grey stripe
    // between two panels reads as a stray border. Painted as the window void
    // (see the Separator rules in the style sheet) the same 5px reads as the
    // gap between panels, and lights up on hover to say it can be dragged.
    //
    // 5px is also roughly the smallest comfortable grab target for a vertical
    // edge with a mouse.
    config.setSeparatorThickness(5);
}

DockWidget* createDockWidget(const QString& uniqueName,
                             const QString& title,
                             QWidget* content,
                             const QIcon& icon)
{
    auto* dock = new DockWidget(uniqueName);
    dock->setTitle(title);

    if (!icon.isNull()) {
        dock->setIcon(icon, KDDockWidgets::IconPlace::All);
    }

    if (content != nullptr) {
        dock->setWidget(content);
    }

    return dock;
}

void setDockIcon(DockWidget* dock, const QIcon& icon)
{
    if (dock != nullptr) {
        dock->setIcon(icon, KDDockWidgets::IconPlace::All);
    }
}

QStringList describeDockChrome(DockWidget* dock)
{
    QStringList lines;
    if (dock == nullptr) {
        return lines;
    }

    // Walk the dock and everything above it up to the group, because the
    // chrome that draws the tab strip and the buttons is a *sibling* of the
    // dock's content, not a child of it.
    QWidget* top = dock;
    for (int step = 0; step < 4 && top->parentWidget() != nullptr; ++step) {
        top = top->parentWidget();
    }

    const QList<QWidget*> widgets = top->findChildren<QWidget*>();

    lines.append(QStringLiteral("root: %1  (%2 descendants)")
                     .arg(QString::fromLatin1(top->metaObject()->className()))
                     .arg(widgets.size()));

    for (const QWidget* widget : widgets) {
        // The class name Qt style sheets actually match on. When this reads
        // "QWidget" for something that is really a KDDockWidgets view, that
        // view has no Q_OBJECT and no class selector can ever reach it.
        const QString className = QString::fromLatin1(widget->metaObject()->className());

        const QPalette& palette = widget->palette();

        lines.append(QStringLiteral("  %1  name='%2'  %3x%4  win=%5 base=%6 light=%7  ss=%8")
                         .arg(className,
                              widget->objectName(),
                              QString::number(widget->width()),
                              QString::number(widget->height()),
                              palette.color(QPalette::Window).name(),
                              palette.color(QPalette::Base).name(),
                              palette.color(QPalette::Light).name(),
                              widget->styleSheet().isEmpty() ? QStringLiteral("-")
                                                             : QStringLiteral("own")));
    }

    return lines;
}

void addDockTo(DockMainWindowBase* window,
               DockWidget* dock,
               DockLocation location,
               QSize initialSize)
{
    if (window == nullptr || dock == nullptr) {
        return;
    }

    if (initialSize.isNull()) {
        window->addDockWidget(dock, toKddwLocation(location));
        return;
    }

    window->addDockWidget(dock, toKddwLocation(location), nullptr,
                          KDDockWidgets::InitialOption{initialSize});
}

QByteArray saveDockLayout()
{
    KDDockWidgets::LayoutSaver saver;
    return saver.serializeLayout();
}

bool restoreDockLayout(const QByteArray& serialized)
{
    if (serialized.isEmpty()) {
        return false;
    }

    KDDockWidgets::LayoutSaver saver;
    return saver.restoreLayout(serialized);
}

KDDockWidgets::Location toKddwLocation(DockLocation location)
{
    switch (location) {
    case DockLocation::Left:   return KDDockWidgets::Location_OnLeft;
    case DockLocation::Right:  return KDDockWidgets::Location_OnRight;
    case DockLocation::Top:    return KDDockWidgets::Location_OnTop;
    case DockLocation::Bottom: return KDDockWidgets::Location_OnBottom;
    }
    return KDDockWidgets::Location_OnRight;
}

} // namespace torquebus::ui
