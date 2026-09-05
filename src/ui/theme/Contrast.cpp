// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/theme/Contrast.h"

#include <algorithm>
#include <cmath>

namespace torquebus::ui {
namespace {

/// Undoes the sRGB transfer function for one channel.
///
/// The kink at 0.03928 is not an approximation of the curve - it is part of the
/// sRGB definition, a linear segment near black that keeps the derivative
/// finite there.
[[nodiscard]] double linearise(double channel)
{
    return channel <= 0.03928 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

} // namespace

double relativeLuminance(const QColor& color)
{
    const QColor rgb = color.toRgb();

    return 0.2126 * linearise(rgb.redF())
         + 0.7152 * linearise(rgb.greenF())
         + 0.0722 * linearise(rgb.blueF());
}

double contrastRatio(const QColor& first, const QColor& second)
{
    const double a = relativeLuminance(first);
    const double b = relativeLuminance(second);

    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

} // namespace torquebus::ui
