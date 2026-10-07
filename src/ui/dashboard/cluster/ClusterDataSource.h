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

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/cluster/ClusterProfile.h"

#include <QObject>
#include <QQmlPropertyMap>
#include <QVariant>

#include <functional>
#include <optional>

namespace torquebus::ui {

class ClusterDataSource final : public QObject {
    Q_OBJECT

public:
    /// The value a binding reads right now, or nothing when it has none: a signal never seen, a
    /// store that is not there, a binding that names nothing.
    using Reader = std::function<std::optional<double>(const DashboardBinding&)>;

    explicit ClusterDataSource(QObject* parent = nullptr);

    /// Which roles come from where. Null leaves every role without a source. Not owned: the
    /// registry's profiles live as long as the program.
    void setProfile(const ClusterProfile* profile);

    /// Reads every role that has a source and publishes it. A role without a source, or whose
    /// source has no value, is published as no data: the cluster draws dashes for it. Publishing
    /// the value a role already has tells QML nothing - QQmlPropertyMap sees to that - so a refresh
    /// in which nothing moved costs the cluster nothing; ClusterDataSourceTests keeps it so.
    void update(const Reader& read);

    /// The object ClusterView.qml reads, with every role in it from the start: a binding in QML
    /// only follows a property that exists when it is first evaluated, so a role added to the map
    /// later would never reach one that read it before.
    [[nodiscard]] QQmlPropertyMap* vehicle() noexcept { return m_vehicle; }

private:
    QQmlPropertyMap* m_vehicle{nullptr}; ///< a child of this
    const ClusterProfile* m_profile{nullptr};
};

} // namespace torquebus::ui
