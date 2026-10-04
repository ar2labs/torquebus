// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/graph/PlotSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace torquebus::ui {

PlotSettingsDialog::PlotSettingsDialog(const PlotSettings& current, QWidget* parent)
    : QDialog{parent}
{
    setWindowTitle(tr("Plot Settings"));
    setMinimumWidth(360);

    auto* formLayout = new QFormLayout();
    formLayout->setSpacing(10);

    // Title
    m_titleEdit = new QLineEdit(this);
    m_titleEdit->setText(current.title);
    m_titleEdit->setPlaceholderText(tr("e.g. MicroMod FD Pots"));
    formLayout->addRow(tr("Plot Title:"), m_titleEdit);

    // Subplot Layout
    m_layoutCombo = new QComboBox(this);
    m_layoutCombo->addItem(tr("Auto Subplots (Group by Unit)"),
                           static_cast<int>(SubplotLayoutMode::AutoByUnit));
    m_layoutCombo->addItem(tr("Single Shared Canvas"),
                           static_cast<int>(SubplotLayoutMode::SinglePlot));
    m_layoutCombo->addItem(tr("Manual Subplots"), static_cast<int>(SubplotLayoutMode::Manual));
    int layoutIdx = m_layoutCombo->findData(static_cast<int>(current.layoutMode));
    if (layoutIdx >= 0) {
        m_layoutCombo->setCurrentIndex(layoutIdx);
    }
    formLayout->addRow(tr("Layout:"), m_layoutCombo);

    // Time Format
    m_timeFormatCombo = new QComboBox(this);
    m_timeFormatCombo->addItem(tr("Relative Seconds (0.000 s)"),
                               static_cast<int>(TimeDisplayFormat::RelativeSeconds));
    m_timeFormatCombo->addItem(tr("Absolute Date and Time"),
                               static_cast<int>(TimeDisplayFormat::AbsoluteDateTime));
    int timeIdx = m_timeFormatCombo->findData(static_cast<int>(current.timeFormat));
    if (timeIdx >= 0) {
        m_timeFormatCombo->setCurrentIndex(timeIdx);
    }
    formLayout->addRow(tr("Time Axis:"), m_timeFormatCombo);

    // Legend & Grid checkboxes
    m_showLegendCheck = new QCheckBox(tr("Show Live Values Legend Overlay"), this);
    m_showLegendCheck->setChecked(current.showLegend);
    formLayout->addRow(QString{}, m_showLegendCheck);

    m_showGridCheck = new QCheckBox(tr("Show Fine Dotted Grid"), this);
    m_showGridCheck->setChecked(current.showGrid);
    formLayout->addRow(QString{}, m_showGridCheck);

    // Default Fill Style
    m_fillCombo = new QComboBox(this);
    m_fillCombo->addItem(tr("None (Line only)"), static_cast<int>(PlotTraceFill::None));
    m_fillCombo->addItem(tr("Hatched (Diagonal)"), static_cast<int>(PlotTraceFill::Hatched));
    m_fillCombo->addItem(tr("Solid (Translucent)"), static_cast<int>(PlotTraceFill::Solid));
    int fillIdx = m_fillCombo->findData(static_cast<int>(current.defaultFill));
    if (fillIdx >= 0) {
        m_fillCombo->setCurrentIndex(fillIdx);
    }
    formLayout->addRow(tr("Default Area Fill:"), m_fillCombo);

    // Line Width
    m_lineWidthSpin = new QDoubleSpinBox(this);
    m_lineWidthSpin->setRange(0.8, 5.0);
    m_lineWidthSpin->setSingleStep(0.2);
    m_lineWidthSpin->setValue(current.defaultLineWidth);
    formLayout->addRow(tr("Line Width:"), m_lineWidthSpin);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(formLayout);
    mainLayout->addSpacing(10);
    mainLayout->addWidget(buttonBox);
}

PlotSettings PlotSettingsDialog::settings() const
{
    PlotSettings result;
    result.title = m_titleEdit->text().trimmed();
    result.layoutMode = static_cast<SubplotLayoutMode>(m_layoutCombo->currentData().toInt());
    result.timeFormat = static_cast<TimeDisplayFormat>(m_timeFormatCombo->currentData().toInt());
    result.showLegend = m_showLegendCheck->isChecked();
    result.showGrid = m_showGridCheck->isChecked();
    result.defaultFill = static_cast<PlotTraceFill>(m_fillCombo->currentData().toInt());
    result.defaultLineWidth = m_lineWidthSpin->value();
    return result;
}

} // namespace torquebus::ui
