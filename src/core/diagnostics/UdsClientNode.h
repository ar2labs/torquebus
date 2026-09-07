// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// UDS as a block: frames in from the bus, frames out to it, and the diagnostic
// conversation in between.
//
// **It owns its ISO-TP connection rather than being wired to one**, and that is
// a decision worth defending because it looks like duplication. The composable
// arrangement - an ISO-TP block whose messages feed a UDS block whose requests
// feed back into the ISO-TP block - is a cycle, and PipelineGraph refuses
// cycles for a good reason: a dataflow graph with one has no order to run its
// nodes in. Breaking the cycle would mean a pass of latency and an edge drawn
// backwards across the canvas, which is worse to look at and worse to explain
// than one block that does both layers.
//
// The ISO-TP block stays, and is the right one to use when what you want *is*
// ISO-TP: a proprietary protocol over it, or a look at the raw messages. This
// block is for the case where the answer to "what is on the other end" is
// "an ECU speaking UDS", which is most of them.
//
//     CAN Channel --frames--> UDS Client --frames--> CAN Transmit
//                                  |
//                          messages v   (responses, for anything downstream)
//
// Requests come from the Diagnostic Console through a DiagnosticSession, from
// the block's own settings, or from an upstream Events edge - a script that
// asks a question, once there is one.

#pragma once

#include "core/can/CanFrame.h"
#include "core/diagnostics/DiagnosticEvent.h"
#include "core/diagnostics/DiagnosticSession.h"
#include "core/diagnostics/UdsClient.h"
#include "core/isotp/IsoTpConnection.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace torquebus {

class UdsClientNode final : public IPipelineNode {
public:
    UdsClientNode(IsoTpAddress address,
                  IsoTpConfig transport,
                  UdsTiming timing,
                  DiagnosticSession* session = nullptr,
                  std::string label = "UDS Client");

    [[nodiscard]] std::string_view typeName() const noexcept override { return "uds.client"; }

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
            {"Requests sent", m_sent},
            {"Answers received", m_answered},
            // The two numbers that say whether an ECU is healthy, and they say
            // different things: a refusal is an ECU that is talking, a timeout
            // is one that is not.
            {"Refused by the ECU", m_refused},
            {"No answer", m_timedOut},
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

    void publish(const UdsExchange& exchange, std::uint64_t nowNs);

    IsoTpConnection m_transport;
    UdsClient m_client;
    DiagnosticSession* m_session{nullptr};
    std::string m_label;

    std::chrono::steady_clock::time_point m_started;

    /// Requests waiting for the client to be free. Unlike the client's own
    /// refusal to queue, this one exists because a console user pressing Send
    /// twice quickly means both, and a millisecond apart is not two questions
    /// at once.
    std::vector<std::vector<std::uint8_t>> m_queued;

    std::vector<CanFrame> m_outgoingFrames;
    std::vector<DiagnosticEvent> m_outgoingEvents;

    std::uint64_t m_sent{0};
    std::uint64_t m_answered{0};
    std::uint64_t m_refused{0};
    std::uint64_t m_timedOut{0};
};

} // namespace torquebus
