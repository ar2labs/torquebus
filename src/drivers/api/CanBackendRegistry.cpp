// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "drivers/api/CanBackendRegistry.h"

#include "drivers/kvaser/KvaserCanBackend.h"
#include "drivers/peak/PeakCanBackend.h"
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

    registerBackend("virtual", "TorqueBus Virtual Bus",
                    [] { return std::make_unique<VirtualCanBackend>(); });

    // Adding Kvaser cost exactly this: one implementation of ICanBackend, and
    // this one line. Nothing above the driver layer changed - which is the
    // whole claim ARCHITECTURE.md makes about the seam, now tested against a
    // real vendor SDK rather than asserted.
    registerBackend("kvaser", "Kvaser CANlib",
                    [] { return std::make_unique<KvaserCanBackend>(); });

    // And PEAK, in one more line just like it - which is the claim the Kvaser
    // comment above made, now kept. Nothing between the two lines changed, and
    // nothing above the driver layer did either.
    registerBackend("peak", "PEAK-System PCAN-Basic",
                    [] { return std::make_unique<PeakCanBackend>(); });
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
