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
    , m_seen(allClusterRoles().size())
{
    // Every key, empty: see vehicle().
    for (const ClusterRole role : allClusterRoles()) {
        m_vehicle->insert(keyOf(role), QVariant{});
    }
}

void ClusterDataSource::setProfile(const ClusterProfile* profile)
{
    m_profile = profile;

    // The old profile's values, and what was known of their age, do not belong to the new one.
    for (const ClusterRole role : allClusterRoles()) {
        m_vehicle->insert(keyOf(role), QVariant{});
    }

    m_seen.assign(m_seen.size(), Seen{});
}

bool ClusterDataSource::isStale(std::size_t index,
                                const ClusterSource& source,
                                const ClusterReading& reading,
                                Clock::time_point now)
{
    Seen& seen = m_seen[index];

    // A value with no stamp has no age.
    if (!reading.stamp.has_value()) {
        seen.has = false;
        return false;
    }

    // A new sample, whatever its stamp is: the stamp is compared for change and not for order, so a
    // store that started over is as fresh as one that went on.
    if (!seen.has || seen.stamp != *reading.stamp) {
        seen = Seen{true, *reading.stamp, now};
        return false;
    }

    return source.maxAgeMs != 0 && now - seen.since > std::chrono::milliseconds{source.maxAgeMs};
}

void ClusterDataSource::update(const Reader& read, Clock::time_point now)
{
    const auto roles = allClusterRoles();

    for (std::size_t i = 0; i < roles.size(); ++i) {
        const ClusterSource* source =
            m_profile != nullptr ? m_profile->sourceOf(roles[i]) : nullptr;
        const std::optional<ClusterReading> reading =
            source != nullptr ? read(source->binding) : std::nullopt;

        QVariant published; // no data, unless there is a number to show

        if (reading.has_value() && std::isfinite(reading->value)
            && !isStale(i, *source, *reading, now)) {
            published = isFlag(roles[i]) ? QVariant{reading->value > kFlagThreshold}
                                         : QVariant{reading->value};
        }

        if (!reading.has_value()) {
            m_seen[i] = Seen{};
        }

        m_vehicle->insert(keyOf(roles[i]), published);
    }
}

} // namespace torquebus::ui
