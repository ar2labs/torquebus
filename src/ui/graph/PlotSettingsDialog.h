// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#pragma once

#include "ui/graph/PlotView.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;

namespace torquebus::ui {

struct PlotSettings {
    QString title;
    SubplotLayoutMode layoutMode{SubplotLayoutMode::AutoByUnit};
    TimeDisplayFormat timeFormat{TimeDisplayFormat::RelativeSeconds};
    bool showLegend{true};
    bool showGrid{true};
    PlotTraceFill defaultFill{PlotTraceFill::None};
    double defaultLineWidth{1.6};
};

class PlotSettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PlotSettingsDialog(const PlotSettings& current, QWidget* parent = nullptr);

    [[nodiscard]] PlotSettings settings() const;

private:
    QLineEdit* m_titleEdit{nullptr};
    QComboBox* m_layoutCombo{nullptr};
    QComboBox* m_timeFormatCombo{nullptr};
    QCheckBox* m_showLegendCheck{nullptr};
    QCheckBox* m_showGridCheck{nullptr};
    QComboBox* m_fillCombo{nullptr};
    QDoubleSpinBox* m_lineWidthSpin{nullptr};
};

} // namespace torquebus::ui
