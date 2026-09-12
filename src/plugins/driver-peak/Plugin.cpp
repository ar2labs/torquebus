// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The PEAK backend, as a plugin.
//
// The same shape as the Kvaser one, which is the point: adding a vendor was one
// implementation of ICanBackend and one registerBackend call when both were
// compiled in, and it is still one implementation and one call now that they
// are loaded. Nothing between the two files differs except the vendor.
//
// This one links Qt, because the backend underneath it reaches PCAN through
// Qt SerialBus. That is allowed and costs nothing: the application already
// loads Qt, and the build key guarantees the plugin and the host agree about
// the compiler and the standard library - which are the things that actually
// decide whether two binaries can pass each other a std::function.

#include "plugins/driver-peak/PeakCanBackend.h"
#include "plugins/host/PluginApi.h"

#include <memory>

namespace {

bool registerWith(const torquebus::plugins::PluginHost& host)
{
    // Built without Qt SerialBus, so this file holds the stub. An interface in
    // the Hardware Configuration list that can never open reads as a broken
    // driver rather than as a build made without the module.
    if (!torquebus::PeakCanBackend::isCompiledIn()) {
        host.log("PEAK: this plugin was built without Qt SerialBus, so it has no "
                 "backend to offer",
                 true);
        return false;
    }

    host.backends->registerBackend("peak", "PEAK-System PCAN-Basic", [] {
        return std::make_unique<torquebus::PeakCanBackend>();
    });

    return true;
}

[[nodiscard]] torquebus::plugins::PluginInfo makeInfo()
{
    torquebus::plugins::PluginInfo info;
    info.name = "peak";
    info.displayName = "PEAK-System PCAN-Basic";
    info.version = "1.0";
    info.registerWith = &registerWith;

    return info;
}

} // namespace

TORQUEBUS_DECLARE_PLUGIN(makeInfo())
