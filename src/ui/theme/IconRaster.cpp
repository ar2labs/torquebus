// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/theme/IconRaster.h"

#include <QPainter>
#include <QSvgRenderer>

namespace torquebus::ui {

QImage tintedSvgImage(QSvgRenderer& renderer, const QColor& color, const QSize& size)
{
    if (size.isEmpty()) {
        return {};
    }

    QImage image{size, QImage::Format_ARGB32_Premultiplied};
    image.fill(Qt::transparent);

    QPainter painter{&image};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(image.rect(), color);
    painter.end();

    return image;
}

} // namespace torquebus::ui
