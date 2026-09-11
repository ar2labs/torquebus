// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/dashboard/SystemVariables.h"

namespace torquebus {

SystemVariables::Handle SystemVariables::resolve(const std::string& name)
{
    const std::lock_guard lock{m_mutex};

    if (const auto found = m_byName.find(name); found != m_byName.end()) {
        return found->second;
    }

    const std::size_t used = m_count.load(std::memory_order_relaxed);

    if (used >= kMaximumVariables) {
        return kUnknown;
    }

    const Handle handle = used;

    m_byName.emplace(name, handle);
    m_order.push_back(name);

    // The slot itself needs no initialisation - it was constructed with the
    // object - so publishing the count is the whole act of creation. Release,
    // paired with the acquire in count(): a reader that can see this handle can
    // see everything that went into making it.
    m_count.store(used + 1, std::memory_order_release);

    return handle;
}

SystemVariables::Handle SystemVariables::find(const std::string& name) const
{
    const std::lock_guard lock{m_mutex};

    const auto found = m_byName.find(name);
    return found == m_byName.end() ? kUnknown : found->second;
}

double SystemVariables::value(Handle handle) const noexcept
{
    // No lock, and none needed: the array is fixed, the slot was published
    // before this handle existed, and the value is an atomic. kUnknown fails
    // the bound like any other out-of-range handle.
    if (handle >= count()) {
        return 0.0;
    }

    return m_slots[handle].value.load(std::memory_order_relaxed);
}

void SystemVariables::set(Handle handle, double value) noexcept
{
    if (handle >= count()) {
        return;
    }

    // Not a const reference: std::atomic::store is not a const member, and the
    // first version of this line did not compile for exactly that reason.
    Slot& slot = m_slots[handle];

    slot.value.store(value, std::memory_order_relaxed);

    // After the value, so a reader that saw a new revision is guaranteed to see
    // the value that came with it.
    slot.revision.fetch_add(1, std::memory_order_release);
}

std::uint64_t SystemVariables::revision(Handle handle) const noexcept
{
    if (handle >= count()) {
        return 0;
    }

    return m_slots[handle].revision.load(std::memory_order_acquire);
}

double SystemVariables::value(const std::string& name) const
{
    return value(find(name));
}

void SystemVariables::set(const std::string& name, double newValue)
{
    set(resolve(name), newValue);
}

std::vector<std::string> SystemVariables::names() const
{
    const std::lock_guard lock{m_mutex};
    return m_order;
}

void SystemVariables::clear()
{
    const std::lock_guard lock{m_mutex};

    m_byName.clear();
    m_order.clear();

    for (Slot& slot : m_slots) {
        slot.value.store(0.0, std::memory_order_relaxed);
        slot.revision.store(0, std::memory_order_relaxed);
    }

    m_count.store(0, std::memory_order_release);
}

} // namespace torquebus
