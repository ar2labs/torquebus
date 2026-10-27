// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The SAE J2534 Pass-Thru backend, as a plugin.

#include "plugins/driver-j2534/J2534CanBackend.h"
#include "plugins/host/PluginApi.h"

#include <memory>

namespace {

bool registerWith(const torquebus::plugins::PluginHost& host)
{
    if (!torquebus::J2534CanBackend::isCompiledIn()) {
        host.log("J2534: this plugin was built without J2534 support", true);
        return false;
    }

    host.backends->registerBackend("j2534", "SAE J2534 Pass-Thru", [] {
        return std::make_unique<torquebus::J2534CanBackend>();
    });

    return true;
}

[[nodiscard]] torquebus::plugins::PluginInfo makeInfo()
{
    torquebus::plugins::PluginInfo info;
    info.name = "j2534";
    info.displayName = "SAE J2534 Pass-Thru";
    info.version = "1.0";
    info.registerWith = &registerWith;

    return info;
}

} // namespace

TORQUEBUS_DECLARE_PLUGIN(makeInfo())
