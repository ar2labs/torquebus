// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/DashboardWidgetEditor.h"

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <limits>

namespace torquebus::ui {
namespace {

/// The three sources, in the combo box, with the enum value beside each.
constexpr int kSourceNone = 0;
constexpr int kSourceSignal = 1;
constexpr int kSourceVariable = 2;

[[nodiscard]] int indexOf(DashboardBinding::Source source)
{
    switch (source) {
    case DashboardBinding::Source::Signal:
        return kSourceSignal;
    case DashboardBinding::Source::Variable:
        return kSourceVariable;
    case DashboardBinding::Source::None:
        break;
    }

    return kSourceNone;
}

} // namespace

DashboardWidgetEditor::DashboardWidgetEditor(DashboardDescription& dashboard, QWidget* parent)
    : QWidget{parent}
    , m_dashboard{dashboard}
{
    buildUi();
    clear();
}

void DashboardWidgetEditor::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 6);
    outer->setSpacing(6);

    m_placeholder = new QLabel(tr("Switch the dashboard to Edit and select a widget."));
    m_placeholder->setProperty("torquebusRole", QStringLiteral("caption"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    outer->addWidget(m_placeholder, 1);

    m_form = new QWidget;
    m_layout = new QFormLayout(m_form);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_kindLabel = new QLabel;
    m_layout->addRow(tr("Widget"), m_kindLabel);

    // The cluster shows dozens of values and is bound to none of them: what it asks for instead is
    // a profile, and the registered ones are the choices. A profile registered later - by a plugin
    // - is listed the next time the editor is built.
    m_profile = new QComboBox;
    for (const ClusterProfile& profile : ClusterProfiles::instance().all()) {
        m_profile->addItem(QString::fromStdString(profile.name),
                           QString::fromStdString(profile.id));
    }
    m_profile->setToolTip(tr("Where each value on the cluster comes from. A value the profile has "
                             "no source for is shown as dashes until one is available."));
    m_layout->addRow(tr("Data profile"), m_profile);

    m_source = new QComboBox;
    m_source->addItem(tr("Nothing"), kSourceNone);
    m_source->addItem(tr("CAN signal"), kSourceSignal);
    m_source->addItem(tr("Variable"), kSourceVariable);
    m_layout->addRow(tr("Shows"), m_source);

    m_message = new QLineEdit;
    m_message->setPlaceholderText(tr("Message name, as the database spells it"));
    m_layout->addRow(tr("Message"), m_message);

    m_signal = new QLineEdit;
    m_signal->setPlaceholderText(tr("Signal name"));
    m_layout->addRow(tr("Signal"), m_signal);

    m_variable = new QLineEdit;
    m_variable->setPlaceholderText(tr("brake_pedal"));
    m_variable->setToolTip(tr("A name a Lua script reads with var_get() and writes "
                              "with var_set(). It does not have to exist yet."));
    m_layout->addRow(tr("Variable"), m_variable);

    m_title = new QLineEdit;
    m_title->setPlaceholderText(tr("Taken from the binding when empty"));
    m_layout->addRow(tr("Title"), m_title);

    m_unit = new QLineEdit;
    m_layout->addRow(tr("Unit"), m_unit);

    const auto makeSpin = [] {
        auto* spin = new QDoubleSpinBox;
        spin->setRange(-1e9, 1e9);
        spin->setDecimals(3);
        return spin;
    };

    m_minimum = makeSpin();
    m_layout->addRow(tr("Minimum"), m_minimum);

    m_maximum = makeSpin();
    m_layout->addRow(tr("Maximum"), m_maximum);

    m_threshold = makeSpin();
    m_threshold->setToolTip(tr("A lamp is on above this; a switch writes the maximum "
                               "when it is turned on and the minimum when it is off."));
    m_layout->addRow(tr("Threshold"), m_threshold);

    m_decimals = new QSpinBox;
    m_decimals->setRange(0, 6);
    m_layout->addRow(tr("Decimals"), m_decimals);

    m_hint = new QLabel;
    m_hint->setWordWrap(true);
    m_hint->setProperty("torquebusRole", QStringLiteral("caption"));
    m_layout->addRow(QString{}, m_hint);

    outer->addWidget(m_form, 1);

    // Every field writes on change. A form that needs a Save button is a form
    // somebody leaves half-applied - and the description is the live one, so
    // there is nothing to commit.
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int) { onFieldChanged(); });
    connect(m_profile, &QComboBox::currentIndexChanged, this, [this](int) { onFieldChanged(); });

    for (QLineEdit* field : {m_message, m_signal, m_variable, m_title, m_unit}) {
        connect(field, &QLineEdit::textEdited, this, [this](const QString&) { onFieldChanged(); });
    }

    for (QDoubleSpinBox* spin : {m_minimum, m_maximum, m_threshold}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double) { onFieldChanged(); });
    }

    connect(m_decimals, &QSpinBox::valueChanged, this, [this](int) { onFieldChanged(); });
}

void DashboardWidgetEditor::clear()
{
    m_widgetId.clear();

    m_form->setVisible(false);
    m_placeholder->setVisible(true);
}

void DashboardWidgetEditor::showWidget(const QString& widgetId)
{
    if (widgetId.isEmpty() || m_dashboard.find(widgetId.toStdString()) == nullptr) {
        clear();
        return;
    }

    m_widgetId = widgetId;

    m_placeholder->setVisible(false);
    m_form->setVisible(true);

    reload();
}

void DashboardWidgetEditor::reload()
{
    const DashboardWidget* widget = m_dashboard.find(m_widgetId.toStdString());
    if (widget == nullptr) {
        clear();
        return;
    }

    // Blocked for the whole repopulation: filling a combo box emits
    // currentIndexChanged, and a form that wrote back while it was being read
    // would save the value it is in the middle of loading.
    const QSignalBlocker blockProfile{m_profile};
    const QSignalBlocker blockSource{m_source};
    const QSignalBlocker blockMessage{m_message};
    const QSignalBlocker blockSignal{m_signal};
    const QSignalBlocker blockVariable{m_variable};
    const QSignalBlocker blockTitle{m_title};
    const QSignalBlocker blockUnit{m_unit};
    const QSignalBlocker blockMinimum{m_minimum};
    const QSignalBlocker blockMaximum{m_maximum};
    const QSignalBlocker blockThreshold{m_threshold};
    const QSignalBlocker blockDecimals{m_decimals};

    const std::string_view kind = nameOf(widget->kind);

    m_kindLabel->setText(QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()))
                         + QStringLiteral("  ·  ") + m_widgetId);

    // -1 for a profile the registry does not have, which validate() refuses at load but a widget
    // can still be in if the registry lost it since: the combo then shows nothing rather than
    // another profile's name.
    m_profile->setCurrentIndex(m_profile->findData(QString::fromStdString(widget->profile)));

    m_source->setCurrentIndex(indexOf(widget->binding.source));

    m_message->setText(QString::fromStdString(widget->binding.message));
    m_signal->setText(QString::fromStdString(widget->binding.signal));
    m_variable->setText(QString::fromStdString(widget->binding.variable));

    m_title->setText(QString::fromStdString(widget->title));
    m_unit->setText(QString::fromStdString(widget->unit));

    m_minimum->setValue(widget->minimum);
    m_maximum->setValue(widget->maximum);
    m_threshold->setValue(widget->threshold);
    m_decimals->setValue(widget->decimals);

    applyVisibility(*widget);
}

void DashboardWidgetEditor::applyVisibility(const DashboardWidget& widget)
{
    const bool isLabel = widget.kind == DashboardWidgetKind::Label;
    const bool isCluster = widget.kind == DashboardWidgetKind::Cluster;

    // Neither has a value of its own to bind or sweep: a label has nothing to show, a cluster has a
    // profile, and everything below this is about one value.
    const bool hasNoValue = isLabel || isCluster;

    const bool control = writesItsBinding(widget.kind);
    const bool signalBound = widget.binding.source == DashboardBinding::Source::Signal;
    const bool variableBound = widget.binding.source == DashboardBinding::Source::Variable;

    const auto showRow = [this](QWidget* field, bool visible) {
        // Row, not field: hiding the editor alone leaves its label behind,
        // pointing at nothing.
        if (QWidget* label = m_layout->labelForField(field); label != nullptr) {
            label->setVisible(visible);
        }
        field->setVisible(visible);
    };

    showRow(m_profile, isCluster);

    // A cluster draws no caption, so a field for one would be a field that does nothing.
    showRow(m_title, !isCluster);

    showRow(m_source, !hasNoValue);
    showRow(m_message, !hasNoValue && signalBound);
    showRow(m_signal, !hasNoValue && signalBound);
    showRow(m_variable, !hasNoValue && variableBound);

    showRow(m_unit,
            !hasNoValue && widget.kind != DashboardWidgetKind::Lamp
                && widget.kind != DashboardWidgetKind::Button
                && widget.kind != DashboardWidgetKind::Switch);

    const bool needsRange = !hasNoValue && widget.kind != DashboardWidgetKind::Lamp;
    showRow(m_minimum, needsRange);
    showRow(m_maximum, needsRange);

    showRow(m_threshold,
            widget.kind == DashboardWidgetKind::Lamp || widget.kind == DashboardWidgetKind::Switch
                || widget.kind == DashboardWidgetKind::Button);

    showRow(m_decimals, !hasNoValue && widget.kind != DashboardWidgetKind::Lamp);

    // The one refusal worth explaining rather than only enforcing: a control
    // bound to a signal moves under the mouse and changes nothing on the bus.
    if (control && signalBound) {
        m_hint->setText(tr("A control cannot write a CAN signal. Bind it to a variable "
                           "and let a script or a transmit entry put that on the bus."));
    } else {
        m_hint->clear();
    }

    m_hint->setVisible(!m_hint->text().isEmpty());
}

void DashboardWidgetEditor::onFieldChanged()
{
    auto* widget = const_cast<DashboardWidget*>(m_dashboard.find(m_widgetId.toStdString()));

    if (widget == nullptr) {
        return;
    }

    switch (m_source->currentData().toInt()) {
    case kSourceSignal:
        widget->binding.source = DashboardBinding::Source::Signal;
        break;
    case kSourceVariable:
        widget->binding.source = DashboardBinding::Source::Variable;
        break;
    default:
        widget->binding.source = DashboardBinding::Source::None;
        break;
    }

    // Only a cluster has one, and only when the combo has an answer: an empty combo is a registry
    // that lost the profile, not a request to clear it.
    if (widget->kind == DashboardWidgetKind::Cluster && m_profile->currentIndex() >= 0) {
        widget->profile = m_profile->currentData().toString().toStdString();
    }

    widget->binding.message = m_message->text().toStdString();
    widget->binding.signal = m_signal->text().toStdString();
    widget->binding.variable = m_variable->text().toStdString();

    widget->title = m_title->text().toStdString();
    widget->unit = m_unit->text().toStdString();

    widget->minimum = m_minimum->value();
    widget->maximum = m_maximum->value();
    widget->threshold = m_threshold->value();
    widget->decimals = m_decimals->value();

    applyVisibility(*widget);

    Q_EMIT widgetEdited(m_widgetId);
}

} // namespace torquebus::ui
