// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The icon set, for QML.
//
// ThemeManager::icon() hands widgets a QIcon in the theme's colour. QML has no QIcon, so
// this does the same job in the shape QML takes: an image provider. An Image asks for
// an icon by name, in a colour, at the size it is going to be drawn:
//
//     image://torquebus-icons/<name>/<colour>
//
//   <name>    a file in resources/icons, without ".svg"
//   <colour>  what QColor reads after a "#": RRGGBB, or AARRGGBB with the alpha first
//
// The colour is part of the address because a QML item changes colour (a lamp comes on)
// and the Image has to fetch the new picture. Qt's pixmap cache keeps each (name, colour,
// size) once, so a lamp that flashes costs one rasterisation, not one per flash.
//
// The pictures are painted by tintedSvgImage(), the same function ThemeManager uses, so
// an icon looks the same in a widget and in QML.

#pragma once

#include <QQuickImageProvider>

namespace torquebus::ui {

/// The name an engine registers the provider under: the host that owns the engine
/// calls engine->addImageProvider(kIconProviderId, new IconImageProvider).
inline constexpr auto kIconProviderId = "torquebus-icons";

class IconImageProvider final : public QQuickImageProvider {
public:
    IconImageProvider();

    /// Runs on Qt Quick's loader thread when an Image is asynchronous, so it touches
    /// nothing that belongs to the GUI thread - no QPixmap, no QIcon.
    ///
    /// A null image means "no such icon" (or a malformed address), and the Image
    /// reports it as an error. `requestedSize` is the Image's sourceSize; when it is
    /// empty the SVG's own size is used.
    [[nodiscard]] QImage
    requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

} // namespace torquebus::ui
