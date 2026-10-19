// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/j1939/J1939Network.h"

#include <algorithm>

namespace torquebus {

const J1939BusView* J1939NetworkSnapshot::bus(std::uint8_t channel) const noexcept
{
    const auto found =
        std::find_if(buses.begin(), buses.end(), [channel](const J1939BusView& view) {
            return view.channel == channel;
        });

    return found != buses.end() ? &*found : nullptr;
}

void J1939Network::publish(J1939BusView view)
{
    const std::lock_guard<std::mutex> guard{m_mutex};

    const auto at = std::lower_bound(
        m_buses.begin(),
        m_buses.end(),
        view.channel,
        [](const J1939BusView& bus, std::uint8_t channel) { return bus.channel < channel; });

    if (at != m_buses.end() && at->channel == view.channel) {
        // Swapped, so that what was published before is freed with the argument, after the lock is
        // released, and not while the other thread waits for it.
        std::swap(*at, view);
    } else {
        // A bus heard for the first time in this measurement: the one allocation the hand-over
        // makes for it under the lock, once.
        m_buses.insert(at, std::move(view));
    }

    m_published = true;

    // Under the lock, after the contents, and with release ordering. A snapshot
    // reads the revision under the same lock, so the revision a copy reports is
    // exactly the publication its contents are from - never a newer one, which
    // a panel that remembers it would take for already shown and not ask again.
    // And a reader that sees the new revision without the lock is guaranteed to
    // see the contents that go with it.
    m_revision.fetch_add(1U, std::memory_order_release);
}

void J1939Network::publish(std::vector<J1939NetworkNode> nodes,
                           std::vector<J1939Defeated> defeated,
                           std::vector<J1939Diagnostic> diagnostics,
                           std::vector<J1939ActiveFault> activeFaults,
                           std::vector<std::uint8_t> reporting,
                           std::uint8_t channel)
{
    J1939BusView view;
    view.channel = channel;
    view.nodes = std::move(nodes);
    view.defeated = std::move(defeated);
    view.diagnostics = std::move(diagnostics);
    view.activeFaults = std::move(activeFaults);
    view.reporting = std::move(reporting);

    publish(std::move(view));
}

J1939NetworkSnapshot J1939Network::snapshot() const
{
    J1939NetworkSnapshot result;

    {
        const std::lock_guard<std::mutex> guard{m_mutex};
        result.buses = m_buses;
        result.published = m_published;

        // Inside the lock: see publish().
        result.revision = m_revision.load(std::memory_order_acquire);
    }

    return result;
}

void J1939Network::clear()
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    m_buses.clear();
    m_lamps.clear();
    m_published = false;

    // Moved rather than reset to zero: a panel watching for a change would miss
    // a clear that put the counter back where it already was, and go on showing
    // the previous run's bus. Under the lock, like a publication.
    m_revision.fetch_add(1U, std::memory_order_release);
}

std::array<std::int8_t, 4> J1939Network::combineLamps(const void* publisher,
                                                      const std::array<std::int8_t, 4>& lit)
{
    const std::lock_guard<std::mutex> guard{m_mutex};

    const auto own = std::find_if(m_lamps.begin(), m_lamps.end(), [publisher](const auto& entry) {
        return entry.first == publisher;
    });

    if (own != m_lamps.end()) {
        own->second = lit;
    } else {
        m_lamps.emplace_back(publisher, lit);
    }

    std::array<std::int8_t, 4> combined{0, 0, 0, 0};

    for (const auto& [block, lamps] : m_lamps) {
        (void)block;

        for (std::size_t lamp = 0; lamp < combined.size(); ++lamp) {
            combined[lamp] = std::max(combined[lamp], lamps[lamp]);
        }
    }

    return combined;
}

} // namespace torquebus
