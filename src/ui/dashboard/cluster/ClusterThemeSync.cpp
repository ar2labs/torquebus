// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/cluster/ClusterThemeSync.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMetaProperty>
#include <QVariant>

#include <array>

namespace torquebus::ui {
namespace {

/// A property of ClusterTheme.qml and the role of Theme it takes its value from. The names
/// are the same on both sides on purpose: a role the cluster uses is a role by that name.
struct Role {
    const char* name;
    QColor Theme::* member;
};

constexpr std::array kRoles{
    Role{"text", &Theme::text},
    Role{"textMuted", &Theme::textMuted},
    Role{"accent", &Theme::accent},
    Role{"accentHover", &Theme::accentHover},
    Role{"border", &Theme::border},
    Role{"canvas", &Theme::canvas},
    Role{"tabStrip", &Theme::tabStrip},
    Role{"warning", &Theme::warning},
    Role{"error", &Theme::error},
    Role{"success", &Theme::success},
    Role{"lampRed", &Theme::lampRed},
    Role{"lampAmber", &Theme::lampAmber},
    Role{"lampGreen", &Theme::lampGreen},
    Role{"lampBlue", &Theme::lampBlue},
    Role{"instrumentScreen", &Theme::instrumentScreen},
    Role{"instrumentBezel", &Theme::instrumentBezel},
};

/// Writes `value` to the writable property `name` of `object`. A property that does not exist
/// is reported rather than created: QObject::setProperty would add a dynamic one, and the QML
/// that was meant to read it would go on seeing the default.
[[nodiscard]] bool writeProperty(QObject& object, const char* name, const QVariant& value)
{
    const QMetaObject* meta = object.metaObject();
    const int index = meta->indexOfProperty(name);

    if (index < 0 || !meta->property(index).isWritable()) {
        qWarning("syncClusterTheme: ClusterTheme.qml has no writable property '%s'.", name);
        return false;
    }

    return meta->property(index).write(&object, value);
}

/// The family a font actually resolves to - the first of its candidates that is installed, which
/// is what the cluster has to name since a QML font takes one family and not a list. Where the
/// platform resolves nothing (the offscreen one the tests run on), the family that was asked for.
[[nodiscard]] QString familyOf(const QFont& font)
{
    const QString resolved = QFontInfo{font}.family();
    return resolved.isEmpty() ? font.family() : resolved;
}

} // namespace

bool syncClusterTheme(QObject& qmlTheme, const Theme& theme)
{
    bool complete = true;

    for (const Role& role : kRoles) {
        complete = writeProperty(qmlTheme, role.name, theme.*role.member) && complete;
    }

    // The same two faces the rest of the application uses. The cluster's big numerals would be
    // better in a condensed face, but shipping one is a decision about licence and size that
    // belongs to the project; until then it reads in the interface font.
    const QString interfaceFamily = familyOf(QGuiApplication::font());
    const QString monoFamily = familyOf(ThemeManager::monospaceFont());

    complete = writeProperty(qmlTheme, "displayFamily", interfaceFamily) && complete;
    complete = writeProperty(qmlTheme, "bodyFamily", interfaceFamily) && complete;
    complete = writeProperty(qmlTheme, "monoFamily", monoFamily) && complete;

    return complete;
}

QStringList clusterThemeColorProperties()
{
    QStringList names;
    names.reserve(static_cast<qsizetype>(kRoles.size()));

    for (const Role& role : kRoles) {
        names.append(QString::fromLatin1(role.name));
    }
    return names;
}

} // namespace torquebus::ui
