// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A panel that exists in the layout but whose module has not landed yet.
//
// Deliberate: the v0.1 window already has the full CANoe/TSMaster panel
// arrangement, so docking, workspaces and layout persistence are exercised
// against the real set of panels from the first commit. Each placeholder is
// replaced in place by its real widget at the milestone named on it - no
// layout surgery, no saved workspace invalidated.

#pragma once

#include <QString>
#include <QWidget>

class QLabel;

namespace torquebus::ui {

class PlaceholderPanel final : public QWidget {
    Q_OBJECT

public:
    /// `iconName` is a resource stem such as "trace"; `milestone` is the
    /// version that will fill the panel in, e.g. "v0.4".
    PlaceholderPanel(QString title,
                     QString description,
                     QString iconName,
                     QString milestone,
                     QWidget* parent = nullptr);

private:
    void rebuild();

    QString m_title;
    QString m_description;
    QString m_iconName;
    QString m_milestone;

    QLabel* m_iconLabel{nullptr};
};

} // namespace torquebus::ui
