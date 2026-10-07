// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/cluster/ClusterDataSource.h"

#include "core/dashboard/cluster/ClusterRole.h"

#include <QString>

#include <cmath>

namespace torquebus::ui {
namespace {

/// On above 0.5, the threshold a Lamp widget starts with.
constexpr double kFlagThreshold = 0.5;

[[nodiscard]] QString keyOf(ClusterRole role)
{
    const std::string_view name = nameOf(role);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

} // namespace

ClusterDataSource::ClusterDataSource(QObject* parent)
    : QObject{parent}
    , m_vehicle{QQmlPropertyMap::create(this)}
{
    // Every key, empty: see vehicle().
    for (const ClusterRole role : allClusterRoles()) {
        m_vehicle->insert(keyOf(role), QVariant{});
    }
}

void ClusterDataSource::setProfile(const ClusterProfile* profile)
{
    m_profile = profile;

    // The old profile's values do not belong to the new one.
    for (const ClusterRole role : allClusterRoles()) {
        m_vehicle->insert(keyOf(role), QVariant{});
    }
}

void ClusterDataSource::update(const Reader& read)
{
    for (const ClusterRole role : allClusterRoles()) {
        const DashboardBinding* binding =
            m_profile != nullptr ? m_profile->sourceOf(role) : nullptr;
        const std::optional<double> value = binding != nullptr ? read(*binding) : std::nullopt;

        QVariant published; // no data, unless there is a number to show

        if (value.has_value() && std::isfinite(*value)) {
            published = isFlag(role) ? QVariant{*value > kFlagThreshold} : QVariant{*value};
        }

        m_vehicle->insert(keyOf(role), published);
    }
}

} // namespace torquebus::ui
