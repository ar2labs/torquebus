// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/theme/IconImageProvider.h"

#include "ui/theme/IconRaster.h"

#include <QColor>
#include <QFile>
#include <QRegularExpression>
#include <QSvgRenderer>

namespace torquebus::ui {
namespace {

/// Nobody asks for an icon the size of a wall on purpose; this stops a mistake in a
/// binding from becoming a gigabyte.
constexpr QSize kLargestImage{1024, 1024};

/// An icon is a lower-case file name in resources/icons. Anything else - a path, a
/// "..", a name from another corner of the resource tree - is refused rather than
/// resolved.
[[nodiscard]] bool isIconName(const QString& name)
{
    static const QRegularExpression pattern{QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$")};
    return pattern.match(name).hasMatch();
}

} // namespace

IconImageProvider::IconImageProvider()
    : QQuickImageProvider{QQuickImageProvider::Image}
{ }

QImage IconImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    const qsizetype slash = id.lastIndexOf(QLatin1Char('/'));
    const QString name = slash < 0 ? QString{} : id.left(slash);
    const QString token = slash < 0 ? QString{} : id.mid(slash + 1);

    // QColor reads "#RRGGBB" and "#AARRGGBB" but also colour *names*; the token is
    // only ever hex digits, so anything else is a mistake and not a request for "red".
    static const QRegularExpression hex{QStringLiteral("^([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")};

    if (!isIconName(name) || !hex.match(token).hasMatch()) {
        qWarning("IconImageProvider: '%s' is not <icon name>/<RRGGBB or AARRGGBB>.",
                 qUtf8Printable(id));
        return {};
    }

    const QColor color{QLatin1Char('#') + token};

    const QString path = QStringLiteral(":/icons/%1.svg").arg(name);
    if (!QFile::exists(path)) {
        qWarning("IconImageProvider: there is no icon '%s'.", qUtf8Printable(name));
        return {};
    }

    QSvgRenderer renderer{path};
    if (!renderer.isValid()) {
        qWarning("IconImageProvider: '%s' is not a valid SVG.", qUtf8Printable(path));
        return {};
    }

    // An icon that is not square (the turn arrow) keeps its proportions inside whatever
    // box was asked for, instead of being stretched to fill it.
    renderer.setAspectRatioMode(Qt::KeepAspectRatio);

    const QSize wanted = requestedSize.isEmpty() ? renderer.defaultSize() : requestedSize;
    const QImage image = tintedSvgImage(renderer, color, wanted.boundedTo(kLargestImage));

    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

} // namespace torquebus::ui
