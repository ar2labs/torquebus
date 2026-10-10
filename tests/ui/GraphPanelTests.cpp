// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/plot/SignalSeries.h"
#include "ui/graph/GraphPanel.h"
#include "ui/graph/PlotSettingsDialog.h"
#include "ui/graph/PlotView.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>

#include <limits>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

void settle()
{
    for (int i = 0; i < 10; ++i) {
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
}

PlotTrace makeSampleTrace(const QString& name, const QString& unit, const QColor& color)
{
    PlotTrace trace;
    trace.name = name;
    trace.unit = unit;
    trace.colour = color;
    trace.minimum = 0.0;
    trace.maximum = 3.0;

    trace.samples.push_back({100'000'000ULL, 1.5});
    trace.samples.push_back({200'000'000ULL, 2.1});
    trace.samples.push_back({300'000'000ULL, 1.8});
    trace.samples.push_back({400'000'000ULL, 2.7});
    return trace;
}

} // namespace

TEST(GraphPanelTests, PlotViewInitialStateAndProperties)
{
    PlotView plot;
    EXPECT_EQ(plot.toolMode(), PlotToolMode::Pointer);
    EXPECT_EQ(plot.subplotLayoutMode(), SubplotLayoutMode::AutoByUnit);
    EXPECT_EQ(plot.timeDisplayFormat(), TimeDisplayFormat::RelativeSeconds);
    EXPECT_TRUE(plot.showLegend());
    EXPECT_TRUE(plot.showGrid());
    EXPECT_FALSE(plot.dualCursorsEnabled());

    plot.setTitle(QStringLiteral("MicroMod FD Pots"));
    EXPECT_EQ(plot.title(), QStringLiteral("MicroMod FD Pots"));

    plot.setWindow(100'000'000ULL, 900'000'000ULL);
    EXPECT_EQ(plot.windowStartNs(), 100'000'000ULL);
    EXPECT_EQ(plot.windowEndNs(), 900'000'000ULL);
}

TEST(GraphPanelTests, SubplotGroupingByUnit)
{
    PlotView plot;
    std::vector<PlotTrace> traces;
    traces.push_back(makeSampleTrace("P0", "V", Qt::red));
    traces.push_back(makeSampleTrace("P1", "V", Qt::yellow));
    traces.push_back(makeSampleTrace("P2", "V", Qt::cyan));

    PlotTrace tank = makeSampleTrace("Tank Level", "l", Qt::green);
    tank.fillStyle = PlotTraceFill::Hatched;
    traces.push_back(tank);

    plot.setTraces(traces);

    // Auto by unit: "V" and "l" should split into 2 subplots
    plot.setSubplotLayoutMode(SubplotLayoutMode::AutoByUnit);
    EXPECT_EQ(plot.subplotCount(), 2);

    // Single shared canvas: 1 subplot
    plot.setSubplotLayoutMode(SubplotLayoutMode::SinglePlot);
    EXPECT_EQ(plot.subplotCount(), 1);
}

TEST(GraphPanelTests, DualMeasurementCursorsAndReadout)
{
    PlotView plot;
    plot.setWindow(0, 1'000'000'000ULL);

    std::vector<PlotTrace> traces;
    traces.push_back(makeSampleTrace("P0", "V", Qt::red));
    plot.setTraces(traces);

    bool measurementFired = false;
    quint64 receivedA = 0;
    quint64 receivedB = 0;
    qint64 receivedDelta = 0;

    QObject::connect(&plot, &PlotView::measurementChanged, [&](quint64 c1, quint64 c2, qint64 d) {
        measurementFired = true;
        receivedA = c1;
        receivedB = c2;
        receivedDelta = d;
    });

    plot.setDualCursorsEnabled(true);
    EXPECT_TRUE(plot.dualCursorsEnabled());
    EXPECT_TRUE(measurementFired);

    plot.setCursorAPosition(200'000'000ULL);
    plot.setCursorBPosition(700'000'000ULL);
    EXPECT_EQ(plot.cursorAPosition(), 200'000'000ULL);
    EXPECT_EQ(plot.cursorBPosition(), 700'000'000ULL);
    EXPECT_EQ(receivedDelta, 500'000'000LL);

    // Interpolation / readout test
    const double valA = plot.valueAtTimestamp(traces.front(), 200'000'000ULL);
    EXPECT_DOUBLE_EQ(valA, 2.1);
}

TEST(GraphPanelTests, ToolModesAndToggles)
{
    PlotView plot;
    plot.setToolMode(PlotToolMode::Pan);
    EXPECT_EQ(plot.toolMode(), PlotToolMode::Pan);

    plot.setToolMode(PlotToolMode::ZoomBox);
    EXPECT_EQ(plot.toolMode(), PlotToolMode::ZoomBox);

    plot.setTimeDisplayFormat(TimeDisplayFormat::AbsoluteDateTime);
    EXPECT_EQ(plot.timeDisplayFormat(), TimeDisplayFormat::AbsoluteDateTime);

    plot.setShowLegend(false);
    EXPECT_FALSE(plot.showLegend());
}

TEST(GraphPanelTests, GraphPanelStoreIntegrationAndPlayback)
{
    GraphPanel panel;
    SignalSeriesStore store{1024};

    CanMessage msgEngine;
    msgEngine.name = "EngineData";

    CanSignal sigSpeed;
    sigSpeed.name = "EngineSpeed";
    sigSpeed.unit = "rpm";

    CanSignal sigTemp;
    sigTemp.name = "CoolantTemp";
    sigTemp.unit = "degC";

    std::vector<DecodedSignal> batch;
    DecodedSignal s1;
    s1.message = &msgEngine;
    s1.signal = &sigSpeed;
    s1.timestampNs = 500'000'000ULL;
    s1.value = 2400.0;
    batch.push_back(s1);

    DecodedSignal s2;
    s2.message = &msgEngine;
    s2.signal = &sigTemp;
    s2.timestampNs = 500'000'000ULL;
    s2.value = 88.5;
    batch.push_back(s2);

    store.append(batch);

    panel.resize(800, 500);
    panel.show();
    panel.setStore(&store);
    settle();

    auto* list = panel.findChild<QListWidget*>("torquebus.list.plotSignals");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count(), 2);

    // Select the first signal
    list->item(0)->setCheckState(Qt::Checked);
    settle();

    PlotView* plot = panel.plotView();
    ASSERT_NE(plot, nullptr);
    EXPECT_EQ(plot->traces().size(), 1);
    EXPECT_EQ(plot->traces().front().name, QStringLiteral("EngineData.EngineSpeed"));
    EXPECT_EQ(plot->traces().front().unit, QStringLiteral("rpm"));
}

TEST(GraphPanelTests, SignalFilterFiltering)
{
    GraphPanel panel;
    SignalSeriesStore store{1024};

    CanMessage msgEngine;
    msgEngine.name = "Engine";
    CanMessage msgBody;
    msgBody.name = "Body";

    CanSignal s1;
    s1.name = "EngineSpeed";
    CanSignal s2;
    s2.name = "TankLevel";

    std::vector<DecodedSignal> batch;
    DecodedSignal d1;
    d1.message = &msgEngine;
    d1.signal = &s1;
    d1.timestampNs = 1000;
    d1.value = 100.0;
    batch.push_back(d1);

    DecodedSignal d2;
    d2.message = &msgBody;
    d2.signal = &s2;
    d2.timestampNs = 1000;
    d2.value = 50.0;
    batch.push_back(d2);

    store.append(batch);
    panel.resize(800, 500);
    panel.show();
    panel.setStore(&store);
    settle();

    auto* filter = panel.findChild<QLineEdit*>("torquebus.input.signalFilter");
    ASSERT_NE(filter, nullptr);
    auto* list = panel.findChild<QListWidget*>("torquebus.list.plotSignals");
    ASSERT_NE(list, nullptr);

    // Filter "Speed"
    filter->setText(QStringLiteral("Speed"));
    settle();

    EXPECT_FALSE(list->item(0)->isHidden()); // Engine.EngineSpeed
    EXPECT_TRUE(list->item(1)->isHidden()); // Body.TankLevel

    // Clear filter
    filter->clear();
    settle();
    EXPECT_FALSE(list->item(0)->isHidden());
    EXPECT_FALSE(list->item(1)->isHidden());
}

TEST(GraphPanelTests, SettingsDialogRoundtrip)
{
    PlotSettings original;
    original.title = QStringLiteral("CANoe Plot");
    original.layoutMode = SubplotLayoutMode::SinglePlot;
    original.timeFormat = TimeDisplayFormat::AbsoluteDateTime;
    original.showLegend = true;
    original.showGrid = false;
    original.defaultFill = PlotTraceFill::Hatched;
    original.defaultLineWidth = 2.4;

    PlotSettingsDialog dlg{original};
    const PlotSettings retrieved = dlg.settings();

    EXPECT_EQ(retrieved.title, QStringLiteral("CANoe Plot"));
    EXPECT_EQ(retrieved.layoutMode, SubplotLayoutMode::SinglePlot);
    EXPECT_EQ(retrieved.timeFormat, TimeDisplayFormat::AbsoluteDateTime);
    EXPECT_TRUE(retrieved.showLegend);
    EXPECT_FALSE(retrieved.showGrid);
    EXPECT_EQ(retrieved.defaultFill, PlotTraceFill::Hatched);
    EXPECT_DOUBLE_EQ(retrieved.defaultLineWidth, 2.4);
}

TEST(GraphPanelTests, ASampleThatIsNotANumberIsAGapInTheLineAndNotASpike)
{
    // A J1939 signal reports "not available" as NaN. The line used to be drawn through it at the
    // middle of the plot - a spike down from the signal's real height to a place it never was, and
    // back - so a sensor that dropped out for a second looked like a reading of half scale.
    PlotView plot;
    plot.resize(600, 300);
    plot.setShowGrid(false);
    plot.setShowLegend(false);
    plot.setWindow(0, 10'000'000'000ULL);

    PlotTrace trace;
    trace.name = "Level";
    trace.colour = Qt::red;
    trace.lineWidth = 2.0;
    trace.minimum = 0.0;
    trace.maximum = 10.0;

    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (std::uint64_t second = 0; second < 10; ++second) {
        const bool missing = second >= 3 && second <= 5;
        trace.samples.push_back(SignalSample{second * 1'000'000'000ULL, missing ? nan : 8.0});
    }

    plot.setTraces({trace});

    const QImage image = plot.grab().toImage();
    ASSERT_FALSE(image.isNull());

    const auto isRed = [&image](int x, int y) {
        const QColor pixel = image.pixelColor(x, y);
        return pixel.red() > 200 && pixel.green() < 80 && pixel.blue() < 80;
    };

    // Rows of the line, at 8 on a scale of 0 to 10, and the rows around the middle, where the NaN
    // would have been drawn.
    int lineRows = 0;
    int middleRows = 0;
    for (int y = 0; y < image.height(); ++y) {
        int red = 0;
        for (int x = 0; x < image.width(); ++x) {
            red += isRed(x, y) ? 1 : 0;
        }

        if (red > 100) {
            ++lineRows; // a row with a long horizontal run of the colour: the line itself
        }
        if (y > image.height() * 40 / 100 && y < image.height() * 60 / 100 && red > 0) {
            ++middleRows;
        }
    }

    EXPECT_GT(lineRows, 0) << "the line is not there at all";
    EXPECT_EQ(middleRows, 0) << "something was drawn at the middle of the plot";
}
