// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The right-hand inspector. Shows whatever the user selected elsewhere,
// as a flat list of name/value rows.
//
// The panel knows nothing about hardware: callers hand it rows. That keeps it
// reusable for databases, signals, transmit entries and diagnostic services
// later on, instead of growing one branch per selectable thing.

#pragma once

#include "core/can/CanTypes.h"

#include <QString>
#include <QVector>
#include <QWidget>

class QTreeWidget;

namespace torquebus::ui {

class PropertiesPanel final : public QWidget {
    Q_OBJECT

public:
    struct Row final {
        QString name;
        QString value;
        bool highlighted{false}; ///< Drawn in the accent colour.
    };

    explicit PropertiesPanel(QWidget* parent = nullptr);

    /// Replaces the contents. An empty `rows` shows the placeholder text.
    void setProperties(const QString& title, const QVector<Row>& rows);

    /// Convenience overload rendering a CAN device, used by the Project
    /// Explorer selection.
    void setDevice(const CanDeviceInfo& device);

    /// Shows "Nothing selected".
    void clearProperties();

private:
    QTreeWidget* m_tree{nullptr};
    QString m_title;
    QVector<Row> m_rows;
};

} // namespace torquebus::ui
