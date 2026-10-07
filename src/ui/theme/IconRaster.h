// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How a monochrome icon becomes pixels in one colour.
//
// The icon set in resources/icons is white on transparent, and the theme decides what
// colour it is on the day. The colour is painted through the alpha channel, so an icon
// is as crisp as the SVG and any colour is one pass away.
//
// Shared by ThemeManager, which wants a QIcon for widgets, and IconImageProvider, which
// wants a QImage for QML. Two copies of this would be two places for the icons to look
// different from each other.

#pragma once

#include <QColor>
#include <QImage>
#include <QSize>

class QSvgRenderer;

namespace torquebus::ui {

/// Renders `renderer`'s SVG over the whole of an image of `size`, then paints `color`
/// through its alpha channel. The result is premultiplied ARGB, as Qt's painters like it.
///
/// Takes a renderer rather than a path because parsing the SVG is the expensive part,
/// and ThemeManager draws the same one at seventy sizes. An empty `size` gives a null
/// image.
[[nodiscard]] QImage tintedSvgImage(QSvgRenderer& renderer, const QColor& color, const QSize& size);

} // namespace torquebus::ui
