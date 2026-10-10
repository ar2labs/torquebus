// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The profiles a cluster can be fed from.
//
// The same shape as CanBackendRegistry and NodeCatalog (rule #10): a new profile is a new entry
// here, and the cluster, the Dashboard and the project file do not change. The built-in profiles
// are in the registry from the start, rather than behind a call that has to be made first: they
// probe nothing, so there is nothing to wait for, and a project file opened before anybody called
// it would otherwise be refused for naming a profile this build has.

#pragma once

#include "core/dashboard/cluster/ClusterProfile.h"

#include <deque>
#include <string_view>

namespace torquebus {

/// The profile a new cluster starts with: the one that works on the project the application ships.
inline constexpr std::string_view kDefaultClusterProfile = "example-11bit";

/// The J1939 commercial vehicle profile, fed from J1939 standard PGNs.
inline constexpr std::string_view kJ1939CommercialProfile = "j1939-commercial";

class ClusterProfiles final {
public:
    /// Process-wide registry, with the built-in profiles already in it.
    [[nodiscard]] static ClusterProfiles& instance();

    /// Registers a profile. Replaces any previous one with the same id, so a plugin can shadow a
    /// built-in profile during development.
    void registerProfile(ClusterProfile profile);

    /// Nullptr when no profile has this id. The pointer stays valid for the life of the program,
    /// whatever is registered afterwards: a cluster holds the profile it reads from, and a plugin
    /// can register another one at any time.
    [[nodiscard]] const ClusterProfile* find(std::string_view id) const noexcept;

    /// In registration order, which is the order the editor lists them in.
    [[nodiscard]] const std::deque<ClusterProfile>& all() const noexcept { return m_profiles; }

private:
    ClusterProfiles();

    // A deque, not a vector: adding to it must not move the profiles already in it.
    std::deque<ClusterProfile> m_profiles;
};

} // namespace torquebus
