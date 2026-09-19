// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Kvaser backend, as a plugin.
//
// This file is the whole difference between a backend that is compiled into
// TorqueBus and one that is loaded into it: an entry point and one
// registerBackend call - the same call the virtual backend makes from inside
// the binary. That is the claim CanBackendRegistry.h made about the seam, now
// kept rather than asserted.
//
// --- Why it is out here at all ----------------------------------------------
//
// PLAN.md section 31: proprietary SDKs do not go into a GPL distribution
// indiscriminately. While this code was linked into torquebus_drivers, the
// application image itself carried a dependency on CANlib. As a plugin, what
// touches CANlib is *loaded* rather than linked, and a machine without the
// Kvaser driver installed simply does not have this file working - which is a
// fact about that machine rather than about the download.

#include "plugins/driver-kvaser/KvaserCanBackend.h"
#include "plugins/host/PluginApi.h"

#include <memory>

namespace {

bool registerWith(const torquebus::plugins::PluginHost& host)
{
    // Built without the SDK, so this file contains the stub that reports itself
    // unavailable. Registering it would put an interface in the Hardware
    // Configuration list that can never open, which reads as a driver problem
    // rather than as a build that was made without the headers.
    if (!torquebus::KvaserCanBackend::isCompiledIn()) {
        host.log("Kvaser: this plugin was built without the CANlib headers, so it "
                 "has no backend to offer",
                 true);
        return false;
    }

    // Registered through the pointer the host handed over, never through
    // CanBackendRegistry::instance(). See PluginApi.h: the registry lives in a
    // static library that both sides link, so instance() here would be a second
    // registry that nobody reads.
    host.backends->registerBackend(
        "kvaser", "Kvaser CANlib", [] { return std::make_unique<torquebus::KvaserCanBackend>(); });

    return true;
}

[[nodiscard]] torquebus::plugins::PluginInfo makeInfo()
{
    torquebus::plugins::PluginInfo info;
    info.name = "kvaser";
    info.displayName = "Kvaser CANlib";
    info.version = "1.0";
    info.registerWith = &registerWith;

    return info;
}

} // namespace

TORQUEBUS_DECLARE_PLUGIN(makeInfo())
