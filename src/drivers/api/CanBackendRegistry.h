// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Where backends announce themselves. The application asks the registry for
// "every channel this machine can offer" without knowing which vendors were
// compiled in, and later the plugin loader will register out-of-tree backends
// through exactly the same call (rule #10).

#pragma once

#include "core/can/CanTypes.h"
#include "drivers/api/ICanBackend.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace torquebus {

/// Creates a fresh, unopened backend instance. One instance drives one channel,
/// so the registry hands out a factory rather than a shared object.
using CanBackendFactory = std::function<std::unique_ptr<ICanBackend>()>;

class CanBackendRegistry final {
public:
    struct Entry final {
        std::string name;         ///< "kvaser", "peak", "virtual"
        std::string displayName;  ///< "Kvaser CANlib"
        CanBackendFactory factory;
        bool available{false};    ///< Vendor SDK present on this machine.
    };

    /// Process-wide registry.
    [[nodiscard]] static CanBackendRegistry& instance();

    /// Registers a backend. Replaces any previous entry with the same name, so
    /// a plugin can shadow a built-in backend during development.
    void registerBackend(std::string name, std::string displayName, CanBackendFactory factory);

    /// Registers the backends compiled into this build (virtual, and Kvaser /
    /// PEAK when enabled). Idempotent.
    void registerBuiltins();

    [[nodiscard]] const std::vector<Entry>& backends() const noexcept { return m_entries; }

    /// Creates an instance of one backend, or nullptr when unknown.
    [[nodiscard]] std::unique_ptr<ICanBackend> create(std::string_view name) const;

    /// Enumerates every channel of every registered backend, in registration
    /// order. This is what the Hardware Manager displays.
    [[nodiscard]] CanDeviceInfoList enumerateAll() const;

private:
    CanBackendRegistry() = default;

    std::vector<Entry> m_entries;
    bool m_builtinsRegistered{false};
};

} // namespace torquebus
