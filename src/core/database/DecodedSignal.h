// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What travels on a Signals edge: one signal, decoded from one frame.
//
// The design question here is what a decoded signal carries about itself. The
// obvious answer is its name, its unit and its value names - and it is the
// wrong one. A batch is a contiguous, non-owning span (see PortType.h), which
// means the payload has to be trivially copyable; a std::string in this struct
// would end that, and at a hundred thousand frames a second it would mean an
// allocation per signal per frame.
//
// So a DecodedSignal points at its definition instead. Names, units and value
// tables are read through `signal` and `message`, which live in the database.
//
// **That makes the database's lifetime part of this type's contract**, and it
// is the one thing to get right when using it: the definitions must outlive
// every batch decoded from them. DbcDecoderNode holds its database by
// shared_ptr for exactly this reason - reloading a .dbc mid-measurement swaps
// in a new one and leaves the old alive until the last node referencing it is
// gone, rather than pulling the definitions out from under a batch in flight.

#pragma once

#include "core/database/CanMessage.h"
#include "core/database/CanSignal.h"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace torquebus {

/// One signal value, decoded from one frame.
struct DecodedSignal final {
    /// Copied from the frame this was decoded from, on the same epoch: the
    /// start of the measurement. Carried per signal rather than per batch
    /// because a batch spans many frames, and a plot needs to know when each
    /// point actually happened.
    std::uint64_t timestampNs{};

    /// The definition. Never null in a batch a decoder produced.
    const CanMessage* message{nullptr};
    const CanSignal* signal{nullptr};

    /// The physical value: raw * factor + offset.
    double value{0.0};

    /// The bits, before the scaling. Kept alongside `value` because it is what
    /// a value table is keyed on, and because an engineer chasing a decode
    /// wants to see the number that came off the wire.
    std::int64_t raw{0};

    std::uint32_t identifier{};

    /// Application channel index, from the frame.
    std::uint8_t channel{};

    /// True when the frame was shorter than the signal needed. The value is
    /// then zero, and this flag is the difference between "the sensor reads
    /// zero" and "we could not read the sensor" - which is exactly the
    /// distinction a decoder that returned only a number would destroy.
    bool truncated{false};

    [[nodiscard]] std::string_view name() const noexcept
    {
        return signal != nullptr ? std::string_view{signal->name} : std::string_view{};
    }

    [[nodiscard]] std::string_view unit() const noexcept
    {
        return signal != nullptr ? std::string_view{signal->unit} : std::string_view{};
    }

    [[nodiscard]] std::string_view messageName() const noexcept
    {
        return message != nullptr ? std::string_view{message->name} : std::string_view{};
    }

    /// The word this raw value stands for, or empty when the database has no
    /// entry covering it.
    [[nodiscard]] std::string_view valueName() const noexcept
    {
        return signal != nullptr ? signal->nameForValue(raw) : std::string_view{};
    }
};

static_assert(std::is_trivially_copyable_v<DecodedSignal>,
              "DecodedSignal must stay trivially copyable: batches are spans of "
              "contiguous storage, and a decoder that had to allocate per signal "
              "could not keep up with a bus.");

} // namespace torquebus
