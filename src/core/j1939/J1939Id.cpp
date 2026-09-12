// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Id.h"

namespace torquebus {

std::optional<J1939Id> j1939Decompose(const CanFrame& frame) noexcept
{
    if (frame.format != CanFrameFormat::Extended) {
        return std::nullopt;
    }

    return j1939Decompose(frame.identifier);
}

} // namespace torquebus
