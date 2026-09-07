// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The other end: an ECU answering UDS.
//
// Everything in this directory so far has been a tester asking questions. This
// is the thing that answers them, and it exists because half the work in a
// vehicle programme is done before there is an ECU to talk to. A supplier's
// tester has to be tried against something; a workshop tool has to be
// demonstrated; a colleague debugging a diagnostic sequence at home has no
// vehicle on the desk. A convincing simulated ECU is the difference between
// that work happening and waiting.
//
// **Convincing means being allowed to say no.** An ECU that answers every
// request positively is not a test target, it is a mirror - it lets a tester
// pass that would fail on the bench the moment it met a locked session, a DID
// the ECU does not have, or a response that takes long enough to need 0x78. So
// the refusals are the point of this class as much as the answers:
//
//   * A DID that does not exist gets requestOutOfRange, not silence.
//   * A DID marked as needing the extended session gets
//     serviceNotSupportedInActiveSession while the ECU is in the default one.
//   * A write to a locked ECU gets securityAccessDenied until the seed and key
//     have been through.
//   * **The session expires.** Five seconds without a TesterPresent and the ECU
//     is back in the default session with its security locked, exactly as a
//     real one does - which is what makes a tester that forgets its heartbeat
//     fail here instead of in a vehicle.
//
// The script - which is what this is ultimately for - gets first refusal on
// every request through a handler, so anything this class does not implement,
// and anything a particular ECU does differently, is a few lines of Lua rather
// than a change here.

#pragma once

#include "core/diagnostics/UdsTypes.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace torquebus {

/// One thing the ECU knows, and what it takes to get at it.
struct UdsIdentifier final {
    std::vector<std::uint8_t> value;

    /// Whether WriteDataByIdentifier is allowed to change it. A read-only DID
    /// answers a write with requestOutOfRange, which is what a real ECU does
    /// with a part number.
    bool writable{false};

    /// The session this DID needs. Default means any.
    UdsSession requiredSession{UdsSession::Default};

    /// Whether security access has to have been granted first.
    bool requiresSecurity{false};
};

class UdsServer final {
public:
    /// What a script decides about one request.
    enum class Verdict : std::uint8_t {
        /// Not this script's business - the server answers as it would have.
        NotHandled,

        /// The script's bytes are the answer.
        Answered,

        /// Say nothing at all. A dead ECU, a busy one, a wire that fell off -
        /// the case a tester has to survive and the one nothing else can
        /// simulate.
        Silent,
    };

    /// Called before anything else, with the request. Fills `response` when it
    /// returns Answered.
    using Handler =
        std::function<Verdict(std::span<const std::uint8_t> request,
                              std::vector<std::uint8_t>& response)>;

    /// Turns a seed into the key the ECU will accept. Without one, security
    /// access is refused - an ECU with no algorithm is locked, not open.
    using SecurityAlgorithm =
        std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t> seed)>;

    UdsServer() = default;

    // --- What the ECU knows ----------------------------------------------

    void setIdentifier(std::uint16_t identifier, UdsIdentifier value);

    [[nodiscard]] const std::map<std::uint16_t, UdsIdentifier>& identifiers() const noexcept
    {
        return m_identifiers;
    }

    /// Adds a stored fault. `status` is the DTC status byte - 0x08 is
    /// "confirmed", which is what a scan tool shows as a stored fault.
    void addTroubleCode(std::uint32_t code, std::uint8_t status = 0x08);

    void clearTroubleCodes();

    [[nodiscard]] const std::vector<DiagnosticTroubleCode>& troubleCodes() const noexcept
    {
        return m_troubleCodes;
    }

    // --- How it behaves ----------------------------------------------------

    void setHandler(Handler handler) { m_handler = std::move(handler); }

    void setSecurityAlgorithm(SecurityAlgorithm algorithm)
    {
        m_algorithm = std::move(algorithm);
    }

    /// The seed the ECU gives out. Fixed rather than random by default, because
    /// a simulated ECU whose seed changes every run cannot be scripted against;
    /// a script that wants a moving seed sets it from its own timer.
    void setSeed(std::vector<std::uint8_t> seed) { m_seed = std::move(seed); }

    /// How long the ECU keeps a non-default session with nothing to hear. Real
    /// ECUs use five seconds, and a tester that forgets its heartbeat should
    /// fail here rather than in a vehicle.
    void setSessionTimeoutMs(std::uint32_t milliseconds) { m_sessionTimeoutMs = milliseconds; }

    // --- The conversation --------------------------------------------------

    /// Answers `request`, or returns nothing when the ECU stays silent.
    ///
    /// Silence is a real answer here: a suppressed TesterPresent, a functional
    /// request this ECU does not implement, or a script that said Silent.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> handle(
        std::span<const std::uint8_t> request,
        std::uint64_t nowNs);

    /// Lets the session expire. Call it as often as convenient.
    void poll(std::uint64_t nowNs);

    [[nodiscard]] UdsSession session() const noexcept { return m_session; }
    [[nodiscard]] bool isUnlocked() const noexcept { return m_unlocked; }

    /// Back to a freshly powered ECU: default session, locked, faults kept.
    /// Faults survive because they do in a real one - that is what stored
    /// means.
    void reset();

private:
    [[nodiscard]] std::vector<std::uint8_t> refuse(std::uint8_t service,
                                                   UdsNegativeResponse reason) const;

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> handleSessionControl(
        std::span<const std::uint8_t> request,
        std::uint64_t nowNs);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> handleReadDid(
        std::span<const std::uint8_t> request);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> handleWriteDid(
        std::span<const std::uint8_t> request);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> handleReadDtc(
        std::span<const std::uint8_t> request);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> handleSecurityAccess(
        std::span<const std::uint8_t> request);

    /// Whether `wanted` is satisfied by the session the ECU is in.
    [[nodiscard]] bool sessionAllows(UdsSession wanted) const noexcept;

    std::map<std::uint16_t, UdsIdentifier> m_identifiers;
    std::vector<DiagnosticTroubleCode> m_troubleCodes;

    Handler m_handler;
    SecurityAlgorithm m_algorithm;

    std::vector<std::uint8_t> m_seed{0x11, 0x22, 0x33, 0x44};

    UdsSession m_session{UdsSession::Default};
    bool m_unlocked{false};

    /// The level whose seed was last handed out, or zero. A key sent without a
    /// seed having been asked for is a sequence error, which is a mistake a
    /// tester makes and should be told about.
    std::uint8_t m_seedGivenFor{0};

    std::uint32_t m_sessionTimeoutMs{5000};
    std::uint64_t m_sessionExpiresNs{0};
};

} // namespace torquebus
