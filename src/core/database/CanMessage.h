// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A message definition and the collection of them that a .dbc file describes.
//
// The lookup here sits on the frame path: every frame that arrives asks the
// database whether it knows it. That is why messages are stored contiguously
// and reached through one hash lookup, rather than through a map of shared
// pointers - at a hundred thousand frames a second the difference is the
// difference between decoding and dropping.

#pragma once

#include "core/can/CanFrame.h"
#include "core/database/CanSignal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace torquebus {

/// One message definition, as a .dbc `BO_` line describes it.
struct CanMessage final {
    /// The identifier, without the format folded in. A .dbc stores the extended
    /// flag in bit 31 of the number on the `BO_` line; the parser splits it out
    /// here so that comparing against a CanFrame is a plain comparison.
    std::uint32_t identifier{};

    CanFrameFormat format{CanFrameFormat::Standard};

    std::string name;

    /// Declared payload length. Frames that carry fewer bytes are still
    /// decoded, signal by signal, for as far as they reach - see
    /// CanSignal::fitsIn.
    std::uint8_t length{8};

    /// The node that sends it, `Vector__XXX` when the database does not say.
    std::string transmitter;

    std::vector<CanSignal> signals;

    /// From a `CM_ BO_` line.
    std::string comment;

    /// From the `GenMsgCycleTime` attribute, in milliseconds. Zero means the
    /// database does not declare one, which is not the same as "sent once" -
    /// plenty of periodic messages carry no attribute at all.
    std::uint32_t cycleTimeMs{0};

    [[nodiscard]] bool matches(const CanFrame& frame) const noexcept
    {
        return frame.identifier == identifier && frame.format == format;
    }

    /// The multiplexer switch signal, when this message has one.
    [[nodiscard]] const CanSignal* multiplexerSwitch() const noexcept;

    /// Signals carried by this frame: every plain signal, plus the multiplexed
    /// ones whose selector value matches what the switch currently reads.
    ///
    /// Returned by pointer into `signals`, so the message must outlive the
    /// result. It is a view, not a copy, because this runs per frame.
    [[nodiscard]] std::vector<const CanSignal*> signalsIn(const std::uint8_t* payload,
                                                          std::size_t payloadLength) const;

    /// An empty frame shaped like this message: identifier, format and declared
    /// length set, payload all zero.
    ///
    /// The starting point for building a frame by signal name, which is what a
    /// transmit panel and a Lua script both need. Zero-filled rather than left
    /// uninitialised because a signal the caller does not set has to be
    /// something definite, and zero is the one value the caller can predict.
    [[nodiscard]] CanFrame makeFrame() const noexcept;

    [[nodiscard]] const CanSignal* findSignal(std::string_view signalName) const noexcept;

    /// The same lookup, for the parser filling in `VAL_` and `CM_` entries
    /// after the signal itself has been read.
    [[nodiscard]] CanSignal* findSignal(std::string_view signalName) noexcept;

    [[nodiscard]] friend bool operator==(const CanMessage&, const CanMessage&) = default;
};

/// The messages of one database, with an index for finding them by identifier.
///
/// A database is a value: it is loaded once, then read from many threads while
/// a measurement runs. Nothing here mutates after `addMessage` returns, which
/// is what makes concurrent decoding safe without a lock.
class CanDatabase final {
public:
    /// From the `VERSION` line. Informational.
    std::string version;

    /// From `BU_:`. Informational, but useful for filtering a trace by sender.
    std::vector<std::string> nodes;

    /// Where it was loaded from, for the UI to show and for error messages.
    std::string sourcePath;

    [[nodiscard]] const std::vector<CanMessage>& messages() const noexcept
    {
        return m_messages;
    }

    [[nodiscard]] std::size_t messageCount() const noexcept { return m_messages.size(); }

    [[nodiscard]] bool empty() const noexcept { return m_messages.empty(); }

    /// Appends a message and indexes it.
    ///
    /// A second message with the same identifier and format replaces the first.
    /// Databases in the wild do contain duplicates, usually from a merge, and
    /// the last definition is the one the tools that produced the file use.
    void addMessage(CanMessage message);

    /// The definition for that identifier, or nullptr.
    ///
    /// The pointer is valid until the next addMessage. On the frame path the
    /// database is already loaded, so that is never.
    [[nodiscard]] const CanMessage* find(std::uint32_t identifier,
                                         CanFrameFormat format) const noexcept;

    [[nodiscard]] const CanMessage* find(const CanFrame& frame) const noexcept
    {
        return find(frame.identifier, frame.format);
    }

    [[nodiscard]] const CanMessage* findByName(std::string_view name) const noexcept;

    /// Total signals across all messages. For the UI to report what it loaded.
    [[nodiscard]] std::size_t signalCount() const noexcept;

    void clear();

private:
    /// Identifier and format packed into one key. The format has to be part of
    /// it: standard 0x123 and extended 0x123 are different messages, and a
    /// database may define both.
    [[nodiscard]] static constexpr std::uint64_t keyFor(std::uint32_t identifier,
                                                        CanFrameFormat format) noexcept
    {
        return (static_cast<std::uint64_t>(format == CanFrameFormat::Extended) << 32U)
               | identifier;
    }

    std::vector<CanMessage> m_messages;
    std::unordered_map<std::uint64_t, std::size_t> m_byIdentifier;
};

} // namespace torquebus
