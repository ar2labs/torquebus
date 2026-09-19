// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The J1939 block: one block, not four.
//
// Reassembly, address watching and diagnostics are three readings of the same
// traffic, and splitting them into separate blocks would mean drawing the same
// wire three times to ask three questions about one bus. So this takes frames
// in and puts decoded signals out - exactly like the DBC decoder, so that the
// Graph panel, the dashboard and the scripts go on receiving what they always
// received - and carries the other two answers alongside, for a panel to read.
//
// --- Matching by PGN, not by identifier --------------------------------------
//
// A J1939 identifier carries the address of whoever transmitted, so the same
// message from two ECUs arrives under two identifiers. Searching a database by
// identifier finds neither. The index built here is by PGN, which is the part
// that says *what* the message is - and the source address travels on as
// information, in the decoded signal, rather than as part of the key.
//
// A database written for one address does not have to be rewritten for a bench
// where the engine answers from a different one, which is the whole point.
//
// --- Transport frames never reach ordinary decoding ---------------------------
//
// A TP.DT is seven bytes of somebody else payload under a sequence number. Fed
// to a signal decoder it produces a full set of plausible numbers, every one of
// them wrong. The reassembler is asked first, and a frame it claims goes no
// further.

#pragma once

#include "core/database/CanMessage.h"
#include "core/database/DecodedSignal.h"
#include "core/j1939/J1939AddressTable.h"
#include "core/j1939/J1939Diagnostics.h"
#include "core/j1939/J1939Network.h"
#include "core/j1939/J1939Transport.h"
#include "core/pipeline/PipelineNode.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace torquebus {

class J1939Node final : public IPipelineNode {
public:
    explicit J1939Node(std::shared_ptr<const CanDatabase> database, std::string label = "J1939")
        : m_database{std::move(database)}
        , m_label{std::move(label)}
    { }

    [[nodiscard]] std::string_view typeName() const noexcept override { return "j1939.decoder"; }

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

    void finish() override;

    [[nodiscard]] std::vector<NodeStatistic> statistics() const override;

    /// Where to hand the view of the bus a panel reads, or nullptr in a
    /// headless build - in which case the block keeps everything to itself and
    /// nothing is copied. Borrowed; it outlives this node.
    void setNetwork(J1939Network* network) noexcept { m_network = network; }

    /// How the SPN field of a trouble code should be read. See
    /// J1939Diagnostics.h - the wrong convention produces a number that looks
    /// like an SPN, which is why this is declared rather than detected.
    void setSpnReading(J1939SpnReading reading) noexcept { m_spnReading = reading; }

    [[nodiscard]] J1939SpnReading spnReading() const noexcept { return m_spnReading; }

    /// Who is on the bus. Written on the executor thread and read by a panel on
    /// its own timer, the same arrangement the test report has.
    [[nodiscard]] const J1939AddressTable& addresses() const noexcept { return m_addresses; }

    /// The most recent DM1 and DM2 from each ECU that has sent one, in address
    /// order. A fault list is a statement about now, so an older copy of the
    /// same ECU list is not worth keeping.
    [[nodiscard]] std::span<const J1939Diagnostic> diagnostics() const noexcept
    {
        return m_diagnostics;
    }

    [[nodiscard]] const std::shared_ptr<const CanDatabase>& database() const noexcept
    {
        return m_database;
    }

    /// How many PGNs the loaded database can name. Zero with a database loaded
    /// means a database that is not J1939 - every message standard-format, or
    /// every one of them a proprietary identifier.
    [[nodiscard]] std::size_t knownPgns() const noexcept { return m_byPgn.size(); }

private:
    static constexpr std::array<PortDescriptor, 1> kInputs{
        PortDescriptor{"frames", PortType::Frames},
    };
    static constexpr std::array<PortDescriptor, 1> kOutputs{
        PortDescriptor{"signals", PortType::Signals},
    };

    /// Decodes `payload` against the message for `pgn`, appending to m_buffer.
    /// Returns how many signals it wrote.
    std::size_t decodeInto(std::uint32_t pgn,
                           std::uint32_t identifier,
                           std::uint8_t channel,
                           std::uint64_t timestampNs,
                           const std::uint8_t* payload,
                           std::size_t length,
                           std::size_t at);

    /// Files a DM1 or DM2, replacing the previous one from that ECU.
    void recordDiagnostic(J1939Diagnostic message);

    std::shared_ptr<const CanDatabase> m_database;
    std::string m_label;

    /// PGN to definition. Built once in prepare(), because a database does not
    /// change under a running graph.
    std::unordered_map<std::uint32_t, const CanMessage*> m_byPgn;

    J1939Transport m_transport;
    J1939AddressTable m_addresses;
    J1939SpnReading m_spnReading{J1939SpnReading::Version4};

    J1939Network* m_network{nullptr};

    /// Something a panel would show changed during this pass. A steady bus
    /// sends the same messages for an hour and changes nothing after the first
    /// second, so this is false almost always and the hand-over costs nothing.
    bool m_networkDirty{false};

    /// Latest per ECU, in address order. DM1 and DM2 are kept apart.
    std::vector<J1939Diagnostic> m_diagnostics;

    /// Reused every pass; the published span points into here, so process()
    /// never resizes it.
    std::vector<DecodedSignal> m_buffer;

    /// Which signals the message being decoded carries. A member for the same
    /// reason m_buffer is: see CanMessage::signalsIn.
    std::vector<const CanSignal*> m_present;

    std::uint64_t m_frames{0};
    std::uint64_t m_decodedMessages{0};
    std::uint64_t m_unknownPgns{0};
    std::uint64_t m_emitted{0};
    std::uint64_t m_transportMessages{0};
    std::uint64_t m_transportFailures{0};
    std::uint64_t m_diagnosticMessages{0};
};

} // namespace torquebus
