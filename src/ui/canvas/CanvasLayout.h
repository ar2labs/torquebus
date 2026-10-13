// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How big a block on the Pipeline canvas is, where its ports are, and where the
// bus is drawn.
//
// In one place because four things have to agree on every number here and none
// of them can ask another: the model says how big a block is, the geometry says
// where its ports are, the painters draw both, and the tests click on what was
// drawn. A block that is 48 high to the model and 46 to the painter is a port
// that cannot be hit.
//
// The canvas draws a block in one of two sizes. A *chip* is what a block is by
// default - its name, what it is, and a line of what it is doing - small enough
// that a project of twenty blocks is one screen. A *card* is what a block is
// when somebody asks to look inside it: the frame it last carried, byte by byte,
// and what the database says that frame means. The bus is neither: it is a
// rail, as long as the modules wired to it are spread out.

#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace torquebus::ui::layout {

// --- Chip ---------------------------------------------------------------------

inline constexpr double kChipWidth = 176.0;
inline constexpr double kChipHeight = 48.0;

/// Room each side of a chip that a neighbour is placed at, so two chips in a row
/// leave a gap a wire can run down.
inline constexpr double kChipGap = 20.0;
inline constexpr double kChipPitch = kChipWidth + kChipGap;

/// The pencil that opens a script block's editor, and the chevron that opens a
/// block into a card, on a chip whose rectangle is `chip`. Where they are drawn and
/// where a click on them is found, which is why neither is written down twice.
[[nodiscard]] inline QRectF chipScriptButton(const QRectF& chip)
{
    return QRectF{chip.right() - 50.0, chip.top() + 7.0, 18.0, 18.0};
}

[[nodiscard]] inline QRectF chipExpandButton(const QRectF& chip)
{
    return QRectF{chip.right() - 28.0, chip.top() + 25.0, 18.0, 16.0};
}

// --- Card ---------------------------------------------------------------------

inline constexpr double kCardWidth = 310.0;
inline constexpr double kCardHeight = 150.0;

/// Where the parts of a card are, measured from its top left. The same numbers
/// the painter draws with and the Edit button is found by.
inline constexpr double kCardHeaderHeight = 24.0;
inline constexpr double kCardFrameTop = 30.0;
inline constexpr double kCardGridTop = 50.0;
inline constexpr double kCardGridCellHeight = 33.0;
inline constexpr double kCardDecodeTop = 88.0;
inline constexpr double kCardDecodeHeight = 54.0;

/// The button that closes a card back to a chip, in the corner of its header.
[[nodiscard]] inline QRectF cardCollapseButton(const QRectF& card)
{
    return QRectF{card.right() - 24.0, card.top() + 3.0, 20.0, 18.0};
}

/// What is painted under a TinyML card: the network it is running.
inline constexpr double kTinyMlCardGap = 8.0;
inline constexpr double kTinyMlCardHeight = 114.0;
inline constexpr double kTinyMlCardMinWidth = 310.0;

// --- Rail (the bus) -----------------------------------------------------------

inline constexpr double kRailHeight = 36.0;

/// The plate at the left of the bus that says which one it is.
inline constexpr double kRailPlateWidth = 208.0;

/// What is left of the wires past the last module, and between the plate and the
/// first.
inline constexpr double kRailInset = 26.0;

/// The shortest a bus is drawn, with nothing wired to it.
inline constexpr double kRailMinWidth = 460.0;

/// The two wires of the bus, each side of the middle.
inline constexpr double kRailWireOffset = 4.5;

/// The two wires of a drop, each side of the middle of the module: CAN_H on the left and CAN_L on
/// the right, as far apart as the bus's own two, so that a drop reads as the pair of the bus
/// carried down to a module. There is room between them for the arrow that says which way frames
/// go.
inline constexpr double kDropWireOffset = kRailWireOffset;

/// How far from a rail a block is put when it is added to the bus, and the room
/// between rows of blocks hanging off one.
inline constexpr double kDropLength = 64.0;
inline constexpr double kRowGap = 56.0;

// --- Ports --------------------------------------------------------------------

/// Where port `index` of `count` is on a block `width` wide and `height` high:
/// along the top for an input, along the bottom for an output, evenly spaced.
[[nodiscard]] inline QPointF
portPoint(bool isInput, std::size_t index, std::size_t count, double width, double height)
{
    // Not "slots": that is a Qt macro, and a variable called one is a puzzle about where the
    // declaration went.
    const double perSide = static_cast<double>(std::max<std::size_t>(count, 1));
    const double x = width * (static_cast<double>(index) + 1.0) / (perSide + 1.0);
    return QPointF{x, isInput ? 0.0 : height};
}

// --- Taps ---------------------------------------------------------------------

/// Where a module's wire meets a rail: straight above or below the middle of the
/// module, kept on the wires and off the plate.
[[nodiscard]] inline double tapX(double moduleCentreX, double railLeft, double railWidth)
{
    const double low = railLeft + kRailPlateWidth + kRailInset * 0.5;
    const double high = railLeft + railWidth - kRailInset * 0.5;

    return std::clamp(moduleCentreX, low, std::max(low, high));
}

// --- Drops --------------------------------------------------------------------

/// QtNodes draws every connection at z = -1, below every block. The half of a drop that is what the
/// bus delivers to a module is drawn a step above the half that is what the module puts on the bus
/// (see `CanvasScene`), so that it is the one painted last - the one with the pill on it - and
/// still below the blocks.
inline constexpr double kDeliveredToModuleZ = -0.5;

/// The middle of a drop - the line its arrows and its pill are on - as the corners of a line, from
/// the side of a module (`x` is its middle, `edge` the y of the side the drop leaves from) to the
/// middle of the bus (`busMiddle`) at `landing`. A module that is not over the part of the rail
/// that has wires on it is reached by a step sideways half way, and not by a wire that meets the
/// plate.
[[nodiscard]] inline std::vector<QPointF>
dropCentre(double x, double edge, double landing, double busMiddle)
{
    if (std::abs(landing - x) < 0.5) {
        return {QPointF{x, edge}, QPointF{x, busMiddle}};
    }

    const double middle = (edge + busMiddle) * 0.5;
    return {QPointF{x, edge},
            QPointF{x, middle},
            QPointF{landing, middle},
            QPointF{landing, busMiddle}};
}

/// One of the two wires of a drop, as the corners of a line, from the side of the module to its own
/// wire of the bus. CAN_H (`high`) is the wire on the left and is joined to the upper wire of the
/// bus; CAN_L is on the right and is joined to the lower one. Whichever of them has the further to
/// go passes the other wire of the bus on the way, as it does on a schematic.
///
/// Every line is horizontal or vertical, and the two wires are the same distance apart all the way,
/// round the step too: the one on the inside of a turn turns first.
[[nodiscard]] inline std::vector<QPointF>
dropWire(bool high, double x, double edge, double landing, double busMiddle)
{
    const double side = high ? -kDropWireOffset : kDropWireOffset;
    const double joinsAt = busMiddle + (high ? -kRailWireOffset : kRailWireOffset);

    std::vector<QPointF> corners{QPointF{x + side, edge}};

    if (std::abs(landing - x) < 0.5) {
        corners.emplace_back(x + side, joinsAt);
        return corners;
    }

    const double travel = edge > busMiddle ? -1.0 : 1.0; // The way from the module to the bus.
    const double heading = landing > x ? 1.0 : -1.0; // The way the step goes.
    const bool inside = side * heading > 0.0;
    const double runAt =
        (edge + busMiddle) * 0.5 + travel * kDropWireOffset * (inside ? -1.0 : 1.0);

    corners.emplace_back(x + side, runAt);
    corners.emplace_back(landing + side, runAt);
    corners.emplace_back(landing + side, joinsAt);
    return corners;
}

} // namespace torquebus::ui::layout
