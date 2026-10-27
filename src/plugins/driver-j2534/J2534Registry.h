// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Enumerates Pass-Thru device installations registered in the Windows Registry.

#pragma once

#include <string>
#include <vector>

namespace torquebus::j2534 {

struct J2534DeviceInfo {
    std::string id;          ///< e.g. "0", "1"
    std::string name;        ///< Display name, e.g. "Tactrix Inc. - OpenPort 2.0"
    std::string vendor;      ///< Vendor name
    std::string libraryPath; ///< Full filesystem path to the J2534 DLL
    bool canSupported{true};
};

/// Reads HKEY_LOCAL_MACHINE\SOFTWARE\PassThruSupport.04.04 to find all installed J2534 devices.
[[nodiscard]] std::vector<J2534DeviceInfo> enumerateJ2534Devices();

} // namespace torquebus::j2534
