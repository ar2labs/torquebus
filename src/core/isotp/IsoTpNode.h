// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// ISO-TP as a block on the canvas.
//
// Thin on purpose: IsoTpConnection is where the protocol lives, and this is the
// adapter that gives it frames, takes its frames back, and turns its events
// into the Events payload the graph carries. Everything hard was already done
// where it could be tested without a graph.
//
// The shape is worth stating, because it is what makes ISO-TP an ordinary part
// of the pipeline rather than a special case:
//
//     CAN Channel ──frames──▶ ISO-TP ──frames──▶ CAN Transmit
//                              │  ▲
//                     messages │  │ requests
//                              ▼  │
//
// Frames in from the bus, frames out to it, messages up, requests down. A DBC
// decoder is wired the same way and so is a Lua ECU; nothing about a diagnostic
// session needs the executor to know what a diagnostic session is (rule #11).
//
// Until the UDS client lands in v0.12 nothing upstream produces requests, so
// the block can also send one of its own: a periodic request typed into its
// settings. That is not scaffolding - a tester that repeats ReadDataByIdentifier
// every 500 ms while somebody drives is one of the two things this tool is for.

#pragma once

#include "core/can/CanFrame.h"
#include "core/diagnostics/DiagnosticEvent.h"
#include "core/isotp/IsoTpConnection.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace torquebus {

class IsoTpNode final : public IPipelineNode {
public:
    IsoTpNode(IsoTpAddress address,
              IsoTpConfig config,
              std::vector<std::uint8_t> periodicRequest = {},
              std::uint32_t requestIntervalMs = 0,
              std::string label = "ISO-TP");

    [[nodiscard]] std::string_view typeName() const noexcept override
    {
        return "isotp.transport";
    }

    [[nodiscard]] std::string displayName() const override { return m_label; }

    [[nodiscard]] std::span<const PortDescriptor> inputs() const noexcept override
    {
        return kInputs;
    }

    [[nodiscard]] std::span<const PortDescriptor> outputs() const noexcept override
    {
        return kOutputs;
    }

    [[nodiscard]] Result prepare(std::size_t maximumBatchSize) override;

    void process(NodeContext& context) override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override
    {
        return {
            {"Messages received", m_received},
            {"Messages sent", m_sent},
            // The number that matters when an ECU has gone quiet: a transfer
            // that failed is a question that was asked and not answered.
            {"Transfers failed", m_failed},
            {"Requests refused", m_refused},
        };
    }

private:
    static constexpr std::array<PortDescriptor, 2> kInputs{
        PortDescriptor{"frames", PortType::Frames},
        PortDescriptor{"requests", PortType::Events},
    };

    static constexpr std::array<PortDescriptor, 2> kOutputs{
        PortDescriptor{"frames", PortType::Frames},
        PortDescriptor{"messages", PortType::Events},
    };

    /// Moves whatever the connection produced into this pass's output buffers.
    void drain(std::uint64_t nowNs);

    IsoTpConnection m_connection;
    std::string m_label;

    std::vector<std::uint8_t> m_periodicRequest;
    std::uint64_t m_intervalNs{0};

    /// When the periodic request goes out next. Measured from prepare(), like
    /// the transmit list's clock, so a request rate and a frame timestamp are
    /// two readings of the same thing.
    std::chrono::steady_clock::time_point m_started;
    std::uint64_t m_nextRequestNs{0};
    bool m_sentFirstRequest{false};

    /// Published every pass; the spans handed out point into these, so they
    /// must survive until the next process() (rule #12).
    std::vector<CanFrame> m_outgoingFrames;
    std::vector<DiagnosticEvent> m_outgoingEvents;

    std::uint64_t m_received{0};
    std::uint64_t m_sent{0};
    std::uint64_t m_failed{0};
    std::uint64_t m_refused{0};
};

} // namespace torquebus
