// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/diagnostics/DiagnosticSession.h"

#include <utility>

namespace torquebus {

void DiagnosticSession::postRequest(std::vector<std::uint8_t> request)
{
    if (request.empty()) {
        return;
    }

    const std::lock_guard lock{m_mutex};
    m_requests.push_back(std::move(request));
}

std::vector<UdsExchange> DiagnosticSession::takeExchanges()
{
    std::vector<UdsExchange> taken;

    const std::lock_guard lock{m_mutex};
    taken.swap(m_exchanges);

    return taken;
}

bool DiagnosticSession::takeRequests(std::vector<std::vector<std::uint8_t>>& out)
{
    // try_lock, never lock. The executor is on the frame path and the thing on
    // the other side of this mutex is a window that may be repainting; waiting
    // here would be exactly the coupling rule #5 forbids. A request picked up
    // one pass later is a millisecond of latency nobody can perceive.
    const std::unique_lock lock{m_mutex, std::try_to_lock};

    if (!lock.owns_lock()) {
        return false;
    }

    for (std::vector<std::uint8_t>& request : m_requests) {
        out.push_back(std::move(request));
    }

    m_requests.clear();
    return true;
}

void DiagnosticSession::publishExchange(const UdsExchange& exchange)
{
    // This one does wait, and it is the one place that can afford to: an
    // exchange completes at most a few times a second, the lock is held for a
    // push_back, and the alternative - dropping an answer because the console
    // happened to be reading - loses the only copy of something somebody asked
    // for.
    const std::lock_guard lock{m_mutex};
    m_exchanges.push_back(exchange);
}

void DiagnosticSession::resetForRun()
{
    const std::lock_guard lock{m_mutex};

    m_requests.clear();
    m_exchanges.clear();

    m_busy.store(false, std::memory_order_relaxed);
    m_active.store(false, std::memory_order_relaxed);
    m_session.store(0x01, std::memory_order_relaxed);
}

} // namespace torquebus
