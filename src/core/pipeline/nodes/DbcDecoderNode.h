// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Frames in, signals out. The node that turns a trace an engineer has to read
// in hex into one they can read in degrees and rpm.
//
// This is the first node in the project whose output port is not Frames, and
// that is the point of it: the type system in PortType.h has been carrying the
// Signals value since v0.5 with nothing producing it. A decoder that emitted
// frames "with signals attached" would have been the easy shape and the wrong
// one - what comes out of a DBC decoder is not CAN traffic, and a plot should
// not be connectable to a channel transmit.
//
// Measured: 23 ns per signal, 184 ns for an eight-signal frame, 5.4M frames/s
// through a graph. The engine sustains 193k frames/s, so a decoder on the hot
// path costs about 3% of the budget - which is the number that says it can sit
// there rather than behind a "decode on demand" switch.

#pragma once

#include "core/database/CanMessage.h"
#include "core/database/DecodedSignal.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace torquebus {

/// Decodes frames against a CanDatabase.
///
/// The database is held by shared_ptr, not by reference. Two reasons, and both
/// are lifetime:
///
///   - a DecodedSignal points at the definitions it came from, so the database
///     has to outlive every batch decoded from it (see DecodedSignal.h);
///   - reloading a .dbc while a measurement runs is an ordinary thing to want,
///     and swapping a shared_ptr leaves the old database alive for as long as
///     anything still holds it. Replacing the contents in place would pull the
///     definitions out from under a batch already in flight.
///
/// Several decoder nodes sharing one database is the normal case - one per
/// branch of the graph - and costs one pointer each.
class DbcDecoderNode final : public IPipelineNode {
public:
    explicit DbcDecoderNode(std::shared_ptr<const CanDatabase> database,
                            std::string label = "DBC decoder")
        : m_database{std::move(database)}
        , m_label{std::move(label)}
    {
    }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "dbc.decoder"; }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    /// Sizes the output buffer for the worst case, which is every frame in a
    /// batch being the message with the most signals.
    ///
    /// Sized exactly rather than capped. A cap would be cheaper and would drop
    /// signals under load - and a decoder that quietly stops decoding when the
    /// bus gets busy is worse than no decoder, because the gap looks like the
    /// signal went away. Allocating once at compile time is what the prepare()
    /// contract exists for.
    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Frames decoded", m_decoded},
            // The one worth reading first. A count that reaches 100% of the
            // traffic is the tell that the wrong database is loaded, and until
            // now there was nowhere it could be seen.
            {"Frames not in the database", m_unknown},
            {"Signals emitted", m_emitted},
            {"Signals in frames too short", m_truncated},
        };
    }

    void process(NodeContext& context) override;

    [[nodiscard]] const std::shared_ptr<const CanDatabase>& database() const noexcept
    {
        return m_database;
    }

    /// Frames whose identifier is in the database.
    [[nodiscard]] std::uint64_t decodedFrames() const noexcept { return m_decoded; }

    /// Frames whose identifier is not.
    ///
    /// Counted rather than logged. On a real bus most traffic is outside any
    /// one database, so this is a number for the statistics panel, not a
    /// stream of warnings. A count that is 100% of the traffic is the tell that
    /// the wrong database is loaded - which is the actual mistake this is here
    /// to make visible.
    [[nodiscard]] std::uint64_t unknownFrames() const noexcept { return m_unknown; }

    [[nodiscard]] std::uint64_t emittedSignals() const noexcept { return m_emitted; }

    /// Signals whose message arrived shorter than the database says.
    [[nodiscard]] std::uint64_t truncatedSignals() const noexcept { return m_truncated; }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"signals", PortType::Signals},
    };

    std::shared_ptr<const CanDatabase> m_database;
    std::string m_label;

    /// Reused every pass. The published span points into here, so it must not
    /// be reallocated between process() calls - which is why prepare() sizes it
    /// for the worst case and process() never resizes.
    std::vector<DecodedSignal> m_buffer;

    std::uint64_t m_decoded{0};
    std::uint64_t m_unknown{0};
    std::uint64_t m_emitted{0};
    std::uint64_t m_truncated{0};
};

} // namespace torquebus
