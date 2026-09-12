// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "drivers/api/CanBackendRegistry.h"

#include "drivers/virtual/VirtualCanBackend.h"

#include <algorithm>
#include <utility>

namespace torquebus {

CanBackendRegistry& CanBackendRegistry::instance()
{
    static CanBackendRegistry registry;
    return registry;
}

void CanBackendRegistry::registerBackend(std::string name,
                                         std::string displayName,
                                         CanBackendFactory factory)
{
    if (!factory) {
        return;
    }

    // Probe availability once, here, so the Hardware Manager never has to
    // construct a backend just to grey out a row.
    bool available = false;
    if (const std::unique_ptr<ICanBackend> probe = factory()) {
        available = probe->isAvailable();
    }

    const auto existing = std::ranges::find_if(
        m_entries, [&name](const Entry& entry) { return entry.name == name; });

    Entry entry{std::move(name), std::move(displayName), std::move(factory), available};

    if (existing != m_entries.end()) {
        *existing = std::move(entry);
    } else {
        m_entries.push_back(std::move(entry));
    }
}

void CanBackendRegistry::registerBuiltins()
{
    if (m_builtinsRegistered) {
        return;
    }
    m_builtinsRegistered = true;

    // The virtual bus, and only the virtual bus.
    //
    // Kvaser and PEAK were two more lines here, and the comment on them said
    // that adding a vendor cost one implementation of ICanBackend and one call
    // to registerBackend. They are plugins now, and they make that same call -
    // from outside the binary, through the pointer the loader hands them. The
    // claim about the seam is the same one; it is now being kept by somebody
    // who is not compiled in, which is the only way to find out whether it was
    // ever true.
    //
    // What is left is the backend that needs nothing installed, so that an
    // application with no plugins still has a bus to run against.
    registerBackend("virtual", "TorqueBus Virtual Bus",
                    [] { return std::make_unique<VirtualCanBackend>(); });
}

std::unique_ptr<ICanBackend> CanBackendRegistry::create(std::string_view name) const
{
    const auto entry = std::ranges::find_if(
        m_entries, [name](const Entry& candidate) { return candidate.name == name; });

    return entry != m_entries.end() ? entry->factory() : nullptr;
}

CanDeviceInfoList CanBackendRegistry::enumerateAll() const
{
    CanDeviceInfoList devices;

    for (const Entry& entry : m_entries) {
        if (!entry.available) {
            continue;
        }

        const std::unique_ptr<ICanBackend> backend = entry.factory();
        if (!backend) {
            continue;
        }

        CanDeviceInfoList found = backend->enumerate();
        devices.insert(devices.end(),
                       std::make_move_iterator(found.begin()),
                       std::make_move_iterator(found.end()));
    }

    return devices;
}

} // namespace torquebus
