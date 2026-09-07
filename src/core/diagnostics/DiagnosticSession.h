// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The console's end of a diagnostic conversation.
//
// The same problem the playback transport had, with the answer shaped
// differently. A person types `22 F1 90` and presses Send in the GUI thread;
// the request has to reach a UDS client running on the executor thread, and the
// answer has to come back. ReplayControl solved its version with atomics, which
// works when the payload is a number. Here the payload is a byte string of
// arbitrary length, and there is no lock-free way to hand one over that is
// simpler than a lock.
//
// So there is a mutex - and the executor **never waits on it**. It tries the
// lock once per pass and moves on if the GUI happens to hold it, picking the
// request up a pass later. A few milliseconds of latency on a request somebody
// typed is not measurable by the person who typed it; a frame path that can
// block on a repainting window is the thing rule #5 exists to prevent.
//
// One conversation at a time, which is what UDS is: request, answer, request.

#pragma once

#include "core/diagnostics/UdsClient.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace torquebus {

class DiagnosticSession final {
public:
    DiagnosticSession() = default;

    DiagnosticSession(const DiagnosticSession&) = delete;
    DiagnosticSession& operator=(const DiagnosticSession&) = delete;
    DiagnosticSession(DiagnosticSession&&) = delete;
    DiagnosticSession& operator=(DiagnosticSession&&) = delete;

    // --- The console's side ----------------------------------------------

    /// Queues a request for the next pass of the graph.
    ///
    /// Blocks only against the executor's single try_lock, which it holds for
    /// the length of a vector move.
    void postRequest(std::vector<std::uint8_t> request);

    /// Takes everything that has completed since the last call.
    [[nodiscard]] std::vector<UdsExchange> takeExchanges();

    /// True while a request is outstanding, so a console can grey out Send
    /// rather than letting somebody queue three questions at an ECU that has
    /// answered none.
    [[nodiscard]] bool isBusy() const noexcept { return m_busy.load(std::memory_order_relaxed); }

    /// True while a UDS block is in the running graph. Without one there is
    /// nothing to send with, and the console says so.
    [[nodiscard]] bool isActive() const noexcept
    {
        return m_active.load(std::memory_order_relaxed);
    }

    /// The session the client believes the ECU is in, as a raw sub-function
    /// byte: 0x01 default, 0x03 extended.
    [[nodiscard]] std::uint8_t sessionType() const noexcept
    {
        return m_session.load(std::memory_order_relaxed);
    }

    // --- The executor's side ----------------------------------------------

    /// Moves any queued requests into `out`, appending. Returns false when the
    /// console held the lock - the requests are still there, and the next pass
    /// will get them.
    bool takeRequests(std::vector<std::vector<std::uint8_t>>& out);

    void publishExchange(const UdsExchange& exchange);

    void setBusy(bool busy) noexcept { m_busy.store(busy, std::memory_order_relaxed); }
    void setActive(bool active) noexcept { m_active.store(active, std::memory_order_relaxed); }

    void setSessionType(std::uint8_t session) noexcept
    {
        m_session.store(session, std::memory_order_relaxed);
    }

    /// Clears everything but leaves nothing running. Called when a measurement
    /// starts, so a console does not open showing the last run's answers.
    void resetForRun();

private:
    /// Guards the two queues only. Held for a move, never for work.
    mutable std::mutex m_mutex;

    std::vector<std::vector<std::uint8_t>> m_requests;
    std::vector<UdsExchange> m_exchanges;

    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_active{false};
    std::atomic<std::uint8_t> m_session{0x01};
};

} // namespace torquebus
