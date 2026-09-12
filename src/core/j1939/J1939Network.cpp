// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Network.h"

namespace torquebus {

void J1939Network::publish(std::vector<J1939NetworkNode> nodes,
                           std::vector<J1939Defeated> defeated,
                           std::vector<J1939Diagnostic> diagnostics)
{
    {
        const std::lock_guard<std::mutex> guard{m_mutex};
        m_nodes = std::move(nodes);
        m_defeated = std::move(defeated);
        m_diagnostics = std::move(diagnostics);
    }

    // After the lock is released, and with release ordering, so a reader that
    // sees the new revision is guaranteed to see the contents that go with it.
    m_revision.fetch_add(1U, std::memory_order_release);
}

J1939NetworkSnapshot J1939Network::snapshot() const
{
    J1939NetworkSnapshot result;

    {
        const std::lock_guard<std::mutex> guard{m_mutex};
        result.nodes = m_nodes;
        result.defeated = m_defeated;
        result.diagnostics = m_diagnostics;
    }

    result.revision = revision();
    return result;
}

void J1939Network::clear()
{
    {
        const std::lock_guard<std::mutex> guard{m_mutex};
        m_nodes.clear();
        m_defeated.clear();
        m_diagnostics.clear();
    }

    // Moved rather than reset to zero: a panel watching for a change would miss
    // a clear that put the counter back where it already was, and go on showing
    // the previous run's bus.
    m_revision.fetch_add(1U, std::memory_order_release);
}

} // namespace torquebus
