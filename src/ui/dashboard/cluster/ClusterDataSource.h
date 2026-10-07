// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// What a QML cluster is shown: the `vehicle` object of ClusterView.qml, kept up to date from a
// profile.
//
// The profile says where each role comes from (a ClusterProfile); the Dashboard knows how to read a
// binding. This is the join, and it is deliberately all it is: no timer of its own and no knowledge
// of where a value is stored, so it is the Dashboard's refresh that drives it and the Dashboard's
// own reader that answers it - a cluster shows exactly what a Gauge bound to the same signal would.
//
// With one difference, and it is the point of an instrument: a Gauge shows the last value a signal
// had for ever, and a cluster stops showing a signal that has stopped arriving (see maxAgeMs in
// ClusterProfile.h). A plot is a record and wants the last sample; a speedometer that holds 90 km/h
// after the bus went quiet is telling a lie.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/cluster/ClusterProfile.h"

#include <QObject>
#include <QQmlPropertyMap>
#include <QVariant>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace torquebus::ui {

/// What the Dashboard's reader answers for one binding.
struct ClusterReading final {
    // Not explicit: a reader with no age to report - a variable - answers with the number.
    ClusterReading(double reading, std::optional<std::uint64_t> sampleStamp = std::nullopt)
        : value{reading}
        , stamp{sampleStamp}
    { }

    double value{0.0};

    /// Which sample this is: the timestamp of the latest one of a CAN signal. The cluster does not
    /// read it as a time - the store's clock is the measurement's, and says nothing about the
    /// present when everything has stopped - but as a change: a signal whose stamp has not moved
    /// for its maxAgeMs has not arrived for that long. Empty for a value with no age, which is
    /// never stale.
    std::optional<std::uint64_t> stamp;
};

class ClusterDataSource final : public QObject {
    Q_OBJECT

public:
    using Clock = std::chrono::steady_clock;

    /// The value a binding reads right now, or nothing when it has none: a signal never seen, a
    /// store that is not there, a binding that names nothing.
    using Reader = std::function<std::optional<ClusterReading>(const DashboardBinding&)>;

    explicit ClusterDataSource(QObject* parent = nullptr);

    /// Which roles come from where. Null leaves every role without a source. Not owned: the
    /// registry's profiles live as long as the program.
    void setProfile(const ClusterProfile* profile);

    /// Reads every role that has a source and publishes it. A role without a source, whose source
    /// has no value, or whose signal has not arrived for its maxAgeMs, is published as no data: the
    /// cluster draws dashes for it. Publishing the value a role already has tells QML nothing -
    /// QQmlPropertyMap sees to that - so a refresh in which nothing moved costs the cluster
    /// nothing; ClusterDataSourceTests keeps it so.
    ///
    /// `now` is the present, passed in so that a test can make two seconds pass without waiting
    /// for them.
    void update(const Reader& read, Clock::time_point now = Clock::now());

    /// The object ClusterView.qml reads, with every role in it from the start: a binding in QML
    /// only follows a property that exists when it is first evaluated, so a role added to the map
    /// later would never reach one that read it before.
    [[nodiscard]] QQmlPropertyMap* vehicle() noexcept { return m_vehicle; }

private:
    /// Whether the role's sample is older than its source allows, as far as this has been watching.
    [[nodiscard]] bool isStale(std::size_t index,
                               const ClusterSource& source,
                               const ClusterReading& reading,
                               Clock::time_point now);

    /// The last sample seen for a role, and when it first was.
    struct Seen final {
        bool has{false};
        std::uint64_t stamp{0};
        Clock::time_point since{};
    };

    QQmlPropertyMap* m_vehicle{nullptr}; ///< a child of this
    const ClusterProfile* m_profile{nullptr};
    std::vector<Seen> m_seen; ///< by index into allClusterRoles()
};

} // namespace torquebus::ui
