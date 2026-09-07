// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A UDS conversation: one request out, one answer back, and the two clocks that
// decide when to stop waiting.
//
// Like IsoTpConnection, this owns no thread and no bus. Messages go in and out
// as byte vectors and time arrives as a parameter, so the whole of the timing -
// which is most of what is hard about UDS - is testable without waiting for any
// of it.
//
// The timing is the part worth stating, because it is where testers go wrong:
//
//   * **P2** is how long the ECU has to answer at all. Fifty milliseconds by
//     default, which sounds brutally short and is what the standard says.
//
//   * **0x78 "response pending" is not an error.** It means the ECU heard the
//     request and needs longer, and it restarts the clock as **P2\***, which is
//     five seconds. It can arrive many times - a routine that takes a minute
//     sends a dozen of them. A tester that treats 0x78 as a failure gives up on
//     every ECU that has to erase a flash sector before answering, which is
//     every ECU worth talking to.
//
//   * **A tester that stops talking loses its session.** An ECU drops back to
//     the default session after S3 - five seconds - of silence, taking the
//     security access and everything else with it. So a session that is not the
//     default one has to be kept alive with TesterPresent, and that is this
//     class's job rather than the caller's, because forgetting it produces a
//     failure that appears minutes later and somewhere else.

#pragma once

#include "core/Result.h"
#include "core/diagnostics/UdsTypes.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace torquebus {

/// What the caller gets back.
struct UdsExchange final {
    enum class Outcome : std::uint8_t {
        /// A positive response arrived. `response` holds it, service byte
        /// included.
        Positive,

        /// The ECU refused, and said why. `negativeResponse` is the reason.
        Negative,

        /// Nothing arrived inside P2 (or P2* after a response pending).
        Timeout,

        /// Something arrived that is not an answer to what was asked.
        Mismatch,
    };

    Outcome outcome{Outcome::Positive};

    /// The request that produced this, so a console can show both halves of the
    /// exchange without keeping its own copy.
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> response;

    std::uint8_t negativeResponse{0};

    /// How long the ECU took, in nanoseconds. The number a diagnostic console
    /// shows next to every line, and the one that says whether an ECU is
    /// healthy long before anything else does.
    std::uint64_t elapsedNs{0};

    /// How many "still working" answers arrived before the real one. Worth
    /// showing: an exchange that took four seconds and needed nine of these is
    /// a different story from one that took four seconds in silence.
    std::uint32_t pendingCount{0};

    /// A sentence for the log, whichever outcome this is.
    [[nodiscard]] std::string describe() const;
};

/// How long this end waits, and how it behaves while nothing is being asked.
struct UdsTiming final {
    /// P2: the ECU's first answer. The standard's default.
    std::uint32_t p2Ms{50};

    /// P2*: the extended deadline after a "response pending".
    std::uint32_t p2StarMs{5000};

    /// S3: how long an ECU keeps a non-default session with nothing to hear.
    /// TesterPresent goes out at a fraction of this.
    std::uint32_t s3Ms{5000};

    /// Send TesterPresent to hold a non-default session open.
    ///
    /// On by default. The alternative is a session that expires while somebody
    /// reads the screen, and a next request that fails for a reason that
    /// happened thirty seconds ago.
    bool keepSessionAlive{true};
};

class UdsClient final {
public:
    explicit UdsClient(UdsTiming timing = {});

    [[nodiscard]] const UdsTiming& timing() const noexcept { return m_timing; }

    /// True while an answer is outstanding.
    [[nodiscard]] bool isBusy() const noexcept { return m_busy; }

    /// The session this client believes the ECU is in.
    ///
    /// A belief, not a fact: it is what the last accepted 0x10 said. An ECU
    /// that reset itself is in the default session and has not told anybody.
    [[nodiscard]] UdsSession session() const noexcept { return m_session; }

    /// Starts an exchange. The message to put on the wire is left in
    /// pendingRequests() for the caller to send.
    ///
    /// Fails while another exchange is outstanding: UDS is one question at a
    /// time on one address, and a queue here would hide an ECU that answered
    /// none of them.
    [[nodiscard]] Result request(std::vector<std::uint8_t> message, std::uint64_t nowNs);

    /// Offers a message that arrived from the transport.
    ///
    /// Returns true when it belonged to this exchange. An unsolicited message -
    /// another tester's answer overheard on a shared identifier, or a response
    /// to a request that already timed out - returns false and is left for
    /// somebody else to make sense of.
    bool onMessage(const std::vector<std::uint8_t>& message, std::uint64_t nowNs);

    /// Lets time pass: fires the timeout, and sends TesterPresent when a
    /// session needs holding open.
    void poll(std::uint64_t nowNs);

    /// Messages to put on the wire, oldest first.
    [[nodiscard]] std::span<const std::vector<std::uint8_t>> pendingRequests() const noexcept
    {
        return m_outgoing;
    }

    void clearPendingRequests() { m_outgoing.clear(); }

    /// Completed exchanges, oldest first.
    [[nodiscard]] std::span<const UdsExchange> exchanges() const noexcept { return m_exchanges; }

    void clearExchanges() { m_exchanges.clear(); }

    /// Forgets the outstanding exchange and the session belief. For a
    /// measurement stopping, not for an error.
    void reset();

private:
    void finish(UdsExchange::Outcome outcome,
                const std::vector<std::uint8_t>& response,
                std::uint8_t negativeResponse,
                std::uint64_t nowNs);

    /// Notices a 0x10 that was accepted, so the session belief and the
    /// TesterPresent heartbeat follow what actually happened rather than what
    /// was asked for.
    void noteSessionChange(const std::vector<std::uint8_t>& response, std::uint64_t nowNs);

    UdsTiming m_timing;

    bool m_busy{false};
    std::vector<std::uint8_t> m_request;
    std::uint8_t m_expectedResponse{0};

    std::uint64_t m_startedNs{0};
    std::uint64_t m_deadlineNs{0};
    std::uint32_t m_pendingCount{0};

    UdsSession m_session{UdsSession::Default};

    /// When the next TesterPresent goes out, or zero when none is needed.
    std::uint64_t m_nextKeepAliveNs{0};

    std::vector<std::vector<std::uint8_t>> m_outgoing;
    std::vector<UdsExchange> m_exchanges;
};

} // namespace torquebus
