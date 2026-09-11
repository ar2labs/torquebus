// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The settings of one dashboard widget: what it is bound to, and how it reads.
//
// A sibling of NodePropertiesEditor, deliberately: a widget on a dashboard and
// a block on the canvas are the same kind of thing from the user's side -
// something selected, described in a form beside it, and written straight back
// into the description. Two panels that answer the same question differently
// would be two things to learn.
//
// It shows nothing at all outside edit mode. A form whose fields cannot be
// applied is a form that invites being filled in.

#pragma once

#include <QString>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace torquebus {
class DashboardDescription;
struct DashboardWidget;
}

namespace torquebus::ui {

class DashboardWidgetEditor final : public QWidget {
    Q_OBJECT

public:
    /// Not owned; must outlive the editor.
    explicit DashboardWidgetEditor(DashboardDescription& dashboard,
                                   QWidget* parent = nullptr);

    /// Shows one widget's settings. An unknown id shows the placeholder.
    void showWidget(const QString& widgetId);

    void clear();

    /// Which widget is on the form, or empty.
    [[nodiscard]] QString currentWidget() const { return m_widgetId; }

    /// Re-reads the widget from the description, for a change that came from
    /// somewhere else - a drag on the panel moves the same widget this form is
    /// showing the size of.
    void refreshFromDescription() { showWidget(m_widgetId); }

Q_SIGNALS:
    /// A setting changed and has already been written into the description.
    void widgetEdited(const QString& widgetId);

private Q_SLOTS:
    void onFieldChanged();

private:
    void buildUi();

    /// Repopulates every field from the widget. Signals are blocked throughout:
    /// filling a combo box emits currentIndexChanged, and a form that writes
    /// back while it is being read would save the value it is about to load.
    void reload();

    /// Shows only the fields the kind actually uses. A Label with a range and a
    /// unit is four questions nobody has to answer.
    void applyVisibility(const DashboardWidget& widget);

    DashboardDescription& m_dashboard;
    QString m_widgetId;

    QLabel* m_placeholder{nullptr};
    QWidget* m_form{nullptr};

    QLabel* m_kindLabel{nullptr};

    QComboBox* m_source{nullptr};
    QLineEdit* m_message{nullptr};
    QLineEdit* m_signal{nullptr};
    QLineEdit* m_variable{nullptr};

    QLineEdit* m_title{nullptr};
    QLineEdit* m_unit{nullptr};

    QDoubleSpinBox* m_minimum{nullptr};
    QDoubleSpinBox* m_maximum{nullptr};
    QDoubleSpinBox* m_threshold{nullptr};
    QSpinBox* m_decimals{nullptr};

    QLabel* m_hint{nullptr};

    /// Rows, so a whole row can be hidden with its label rather than leaving a
    /// labelled gap where a field used to be.
    QFormLayout* m_layout{nullptr};
};

} // namespace torquebus::ui
