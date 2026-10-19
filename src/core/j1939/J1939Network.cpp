// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Network.h"

namespace torquebus {

void J1939Network::publish(std::vector<J1939NetworkNode> nodes,
                           std::vector<J1939Defeated> defeated,
                           std::vector<J1939Diagnostic> diagnostics,
                           std::vector<J1939ActiveFault> activeFaults)
{
    const std::lock_guard<std::mutex> guard{m_mutex};

    // Swapped, so that what was published before is freed with the arguments, after the lock is
    // released, and not while the other thread waits for it.
    m_nodes.swap(nodes);
    m_defeated.swap(defeated);
    m_diagnostics.swap(diagnostics);
    m_activeFaults.swap(activeFaults);
    m_published = true;

    // Under the lock, after the contents, and with release ordering. A snapshot
    // reads the revision under the same lock, so the revision a copy reports is
    // exactly the publication its contents are from - never a newer one, which
    // a panel that remembers it would take for already shown and not ask again.
    // And a reader that sees the new revision without the lock is guaranteed to
    // see the contents that go with it.
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
        result.activeFaults = m_activeFaults;
        result.published = m_published;

        // Inside the lock: see publish().
        result.revision = m_revision.load(std::memory_order_acquire);
    }

    return result;
}

void J1939Network::clear()
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    m_nodes.clear();
    m_defeated.clear();
    m_diagnostics.clear();
    m_activeFaults.clear();
    m_published = false;

    // Moved rather than reset to zero: a panel watching for a change would miss
    // a clear that put the counter back where it already was, and go on showing
    // the previous run's bus. Under the lock, like a publication.
    m_revision.fetch_add(1U, std::memory_order_release);
}

} // namespace torquebus
