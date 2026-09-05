// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/transmit/TransmitList.h"

#include <algorithm>

namespace torquebus {
namespace {

/// Microseconds between sends, clamped to something a dispatch loop can hold.
[[nodiscard]] std::uint64_t periodMicroseconds(const TransmitEntry& entry) noexcept
{
    const std::uint32_t cycle = std::max(entry.cycleMs, TransmitList::kMinimumCycleMs);
    return static_cast<std::uint64_t>(cycle) * 1000ULL;
}

} // namespace

std::size_t TransmitList::add(TransmitEntry entry)
{
    const std::lock_guard guard{m_mutex};

    m_entries.push_back(std::move(entry));

    // Due immediately. A row switched on mid-measurement should start now, not
    // one full period from now - waiting a second before the first frame of a
    // 1 Hz message looks exactly like a list that is not working.
    m_nextDueUs.push_back(0);

    return m_entries.size() - 1;
}

void TransmitList::update(std::size_t index, TransmitEntry entry)
{
    const std::lock_guard guard{m_mutex};

    if (index >= m_entries.size()) {
        return;
    }

    // The counters belong to the run, not to the row the panel is holding. A
    // panel that read a row, let the user change a byte, and wrote it back
    // would otherwise rewind sentCount to whatever it was when the dialog
    // opened.
    entry.sentCount = m_entries[index].sentCount;
    entry.lastSentUs = m_entries[index].lastSentUs;

    m_entries[index] = std::move(entry);
}

void TransmitList::remove(std::size_t index)
{
    const std::lock_guard guard{m_mutex};

    if (index >= m_entries.size()) {
        return;
    }

    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
    m_nextDueUs.erase(m_nextDueUs.begin() + static_cast<std::ptrdiff_t>(index));
}

void TransmitList::setEnabled(std::size_t index, bool enabled)
{
    const std::lock_guard guard{m_mutex};

    if (index >= m_entries.size()) {
        return;
    }

    m_entries[index].enabled = enabled;

    // Re-enabling starts the period again from now rather than counting the
    // time the row spent switched off as elapsed.
    if (enabled) {
        m_nextDueUs[index] = 0;
    }
}

void TransmitList::clear()
{
    const std::lock_guard guard{m_mutex};

    m_entries.clear();
    m_nextDueUs.clear();
}

std::size_t TransmitList::size() const
{
    const std::lock_guard guard{m_mutex};
    return m_entries.size();
}

std::vector<TransmitEntry> TransmitList::entries() const
{
    const std::lock_guard guard{m_mutex};
    return m_entries;
}

bool TransmitList::entryAt(std::size_t index, TransmitEntry& out) const
{
    const std::lock_guard guard{m_mutex};

    if (index >= m_entries.size()) {
        return false;
    }

    out = m_entries[index];
    return true;
}

bool TransmitList::sendOnce(const CanFrame& frame)
{
    // No mutex: this is the path a button press takes, and it must not be able
    // to block the executor behind a lock the UI thread happens to hold.
    return m_oneShots.push(frame);
}

bool TransmitList::sendOnce(std::size_t index)
{
    TransmitEntry entry;
    if (!entryAt(index, entry) || !entry.enabled) {
        return false;
    }

    return sendOnce(entry.frame);
}

void TransmitList::collectDue(std::uint64_t nowUs, std::vector<CanFrame>& out)
{
    // One-shots first, and outside the lock. They were asked for explicitly, so
    // they go out ahead of anything the schedule happens to owe.
    CanFrame queued;
    while (m_oneShots.drain(std::span<CanFrame>{&queued, 1}) == 1) {
        out.push_back(queued);
    }

    const std::lock_guard guard{m_mutex};

    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        TransmitEntry& entry = m_entries[index];

        if (!entry.enabled || !entry.isPeriodic()) {
            continue;
        }

        if (nowUs < m_nextDueUs[index]) {
            continue;
        }

        out.push_back(entry.frame);

        entry.lastSentUs = nowUs;
        ++entry.sentCount;

        // From now, not from when it was due.
        //
        // Advancing by adding a period to the previous due time is the textbook
        // way to avoid drift, and it is wrong here: a pass that arrives late -
        // because the bus was saturated, or the window was being dragged -
        // would leave the entry owing several sends, and the list would fire
        // them back to back to catch up. A burst of a message meant to arrive
        // every 100 ms is worse for whatever is receiving it than the drift
        // would have been.
        m_nextDueUs[index] = nowUs + periodMicroseconds(entry);
    }
}

void TransmitList::restartSchedule()
{
    const std::lock_guard guard{m_mutex};

    std::fill(m_nextDueUs.begin(), m_nextDueUs.end(), 0);

    for (TransmitEntry& entry : m_entries) {
        entry.sentCount = 0;
        entry.lastSentUs = 0;
    }
}

} // namespace torquebus
