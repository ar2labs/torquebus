// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// How the newer Dashboard widgets draw themselves, looked at in their pixels.
//
// A painter is given a painter, a rectangle and a reading, so it can be given a QImage, and what it
// draws can be asked for in colours: this point of the bar's groove is the accent colour, that one
// is the groove. That is a stronger statement than "it did not crash", and a weaker one than a
// golden image, which would break on every font and every change of the theme - so the assertions
// are about the things the widget is *for*: how much of a bar is filled, what colour a state is
// tinted, whether the picture the widget was given is the one that appears.
//
// Text is not looked at. The offscreen platform may or may not have the fonts a real screen has,
// and what the words say is checked where they are made (formatReading), not where they are
// painted.

#include "core/dashboard/DashboardAssets.h"
#include "core/dashboard/DashboardDescription.h"
#include "core/pipeline/GraphDescription.h"
#include "core/plot/SignalSeries.h"
#include "core/transmit/TransmitList.h"
#include "services/ProjectFile.h"
#include "ui/dashboard/DashboardImages.h"
#include "ui/dashboard/DashboardWidgetPainters.h"
#include "ui/theme/Theme.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

constexpr int kWidth = 260;
constexpr int kHeight = 200;

/// A canvas the colour the Dashboard's own is, so that what is drawn on it is seen against the
/// right thing.
[[nodiscard]] QImage canvasFor(const Theme& theme, int width = kWidth, int height = kHeight)
{
    QImage image{width, height, QImage::Format_ARGB32_Premultiplied};
    image.fill(theme.canvas);
    return image;
}

/// Whether the pixel at (x, y) is `expected`, give or take `tolerance` in each channel. The edge of
/// an antialiased shape is neither colour, so every assertion is made well inside one.
[[nodiscard]] ::testing::AssertionResult
isColour(const QImage& image, double x, double y, const QColor& expected, int tolerance = 3)
{
    const QColor actual = image.pixelColor(static_cast<int>(x), static_cast<int>(y));

    const bool same = std::abs(actual.red() - expected.red()) <= tolerance
                      && std::abs(actual.green() - expected.green()) <= tolerance
                      && std::abs(actual.blue() - expected.blue()) <= tolerance;

    if (same) {
        return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure()
           << "pixel (" << x << ", " << y << ") is " << actual.name().toStdString() << ", expected "
           << expected.name().toStdString();
}

/// What `tone` over `background` at `alpha` (0 to 255) comes out as.
[[nodiscard]] QColor blended(const QColor& background, const QColor& tone, int alpha)
{
    const double a = alpha / 255.0;

    return QColor{static_cast<int>(std::lround(background.red() * (1.0 - a) + tone.red() * a)),
                  static_cast<int>(std::lround(background.green() * (1.0 - a) + tone.green() * a)),
                  static_cast<int>(std::lround(background.blue() * (1.0 - a) + tone.blue() * a))};
}

/// How many pixels of `image` inside `area` are `colour` to within `tolerance`.
[[nodiscard]] int
countOf(const QImage& image, const QRect& area, const QColor& colour, int tolerance = 12)
{
    int count = 0;

    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);

            if (std::abs(pixel.red() - colour.red()) <= tolerance
                && std::abs(pixel.green() - colour.green()) <= tolerance
                && std::abs(pixel.blue() - colour.blue()) <= tolerance) {
                ++count;
            }
        }
    }

    return count;
}

[[nodiscard]] DashboardReading known(double value)
{
    DashboardReading reading;
    reading.value = value;
    reading.known = true;
    return reading;
}

/// A picture of one colour, written to `directory`.
void writePicture(const QDir& directory, const QString& name, const QColor& colour, int side = 40)
{
    QImage picture{side, side, QImage::Format_ARGB32};
    picture.fill(colour);

    ASSERT_TRUE(picture.save(directory.filePath(name)));
}

struct Scene final {
    Theme theme{Theme::dark()};
    DashboardImages images;
    QSize size{kWidth, kHeight};
    QImage image{canvasFor(theme)};
    QRectF rect{0.0, 0.0, 220.0, 64.0};

    /// Draws with `paint` and leaves the result in `image`.
    template<typename Painter>
    void draw(const DashboardWidget& widget, const DashboardReading& reading, Painter&& paint)
    {
        image = canvasFor(theme, size.width(), size.height());

        QPainter painter{&image};
        const PaintContext context{
            theme, widget, rect, reading, QStringLiteral("caption"), images, false};
        paint(painter, context);
    }
};

[[nodiscard]] DashboardWidget barWidget(double minimum = 0.0, double maximum = 100.0)
{
    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Bar);
    widget.id = "bar";
    widget.minimum = minimum;
    widget.maximum = maximum;
    return widget;
}

} // namespace

// ---------------------------------------------------------------------------
// Words
// ---------------------------------------------------------------------------

TEST(DashboardPaintersTests, AReadingIsSaidAsDashesAWordOrANumberWithItsUnit)
{
    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Numeric);
    widget.unit = "rpm";
    widget.decimals = 1;

    EXPECT_EQ(formatReading(widget, DashboardReading{}).toStdString(), "--");
    EXPECT_EQ(formatReading(widget, known(1500.0)).toStdString(), "1500.0 rpm");
    EXPECT_EQ(formatReading(widget, known(1500.26)).toStdString(), "1500.3 rpm");

    widget.decimals = 0;
    EXPECT_EQ(formatReading(widget, known(1500.6)).toStdString(), "1501 rpm");

    widget.unit.clear();
    EXPECT_EQ(formatReading(widget, known(7.0)).toStdString(), "7");

    // A name beats a number, wherever the widget has one for the value.
    DashboardState reverse;
    reverse.value = -1.0;
    reverse.label = "Reverse";
    widget.states = {reverse};

    EXPECT_EQ(formatReading(widget, known(-1.0)).toStdString(), "Reverse");
    EXPECT_EQ(formatReading(widget, known(2.0)).toStdString(), "2");

    // And a stale reading is dashes, like one that never was.
    DashboardReading stale;
    stale.stale = true;
    EXPECT_EQ(formatReading(widget, stale).toStdString(), "--");
}

TEST(DashboardPaintersTests, ATonesColourComesFromTheThemeAndNowhereElse)
{
    for (const Theme& theme : {Theme::dark(), Theme::light()}) {
        EXPECT_EQ(toneColor(theme, DashboardTone::Neutral), theme.text);
        EXPECT_EQ(toneColor(theme, DashboardTone::Ok), theme.success);
        EXPECT_EQ(toneColor(theme, DashboardTone::Info), theme.accent);
        EXPECT_EQ(toneColor(theme, DashboardTone::Warning), theme.lampAmber);
        EXPECT_EQ(toneColor(theme, DashboardTone::Danger), theme.lampRed);
        EXPECT_EQ(toneColor(theme, DashboardTone::Off), theme.textMuted);
    }

    EXPECT_EQ(withAlpha(QColor{10, 20, 30}, 99).alpha(), 99);
    EXPECT_EQ(withAlpha(QColor{10, 20, 30}, 999).alpha(), 255);
    EXPECT_EQ(withAlpha(QColor{10, 20, 30}, -5).alpha(), 0);

    EXPECT_DOUBLE_EQ(fractionOf(50.0, 0.0, 100.0), 0.5);
    EXPECT_DOUBLE_EQ(fractionOf(-5.0, 0.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(fractionOf(500.0, 0.0, 100.0), 1.0);
    EXPECT_DOUBLE_EQ(fractionOf(5.0, 10.0, 10.0), 0.0);
    EXPECT_DOUBLE_EQ(fractionOf(std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0), 0.0);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

TEST(DashboardPaintersTests, TheGeometryStaysInsideTheWidgetWhateverItsShape)
{
    for (const QSizeF size : {QSizeF{220, 64}, QSizeF{64, 220}, QSizeF{40, 30}, QSizeF{300, 300}}) {
        const QRectF rect{10.0, 20.0, size.width(), size.height()};

        SCOPED_TRACE(::testing::Message() << size.width() << "x" << size.height());

        EXPECT_TRUE(rect.contains(barTrackRect(rect)));
        EXPECT_TRUE(rect.contains(stateChipRect(rect)));
        EXPECT_TRUE(rect.contains(trendPlotRect(rect)));

        EXPECT_GT(barTrackRect(rect).width(), 0.0);
        EXPECT_GT(barTrackRect(rect).height(), 0.0);
        EXPECT_GT(stateChipRect(rect).height(), 0.0);
        EXPECT_GT(trendPlotRect(rect).height(), 0.0);
    }

    // A bar lies along its longer side.
    EXPECT_GT(barTrackRect(QRectF{0, 0, 220, 64}).width(),
              barTrackRect(QRectF{0, 0, 220, 64}).height());
    EXPECT_GT(barTrackRect(QRectF{0, 0, 64, 220}).height(),
              barTrackRect(QRectF{0, 0, 64, 220}).width());
}

TEST(DashboardPaintersTests, ATableShowsAsManyRowsAsFitAndLeavesRoomToSayHowManyAreNot)
{
    const QRectF rect{0.0, 0.0, 260.0, kTableTitleHeight + 4.0 + kTableRowHeight * 5.0};

    EXPECT_EQ(tableRowCapacity(rect, 3), 3);
    EXPECT_EQ(tableRowCapacity(rect, 5), 5);

    // Six do not fit in the room of five, and the fifth's place goes to the line that counts them.
    EXPECT_EQ(tableRowCapacity(rect, 6), 4);
    EXPECT_EQ(tableRowCapacity(rect, 500), 4);

    // And a box too short for any of them shows none, and does not go negative.
    EXPECT_EQ(tableRowCapacity(QRectF{0, 0, 260, 20}, 10), 0);
    EXPECT_EQ(tableRowCapacity(rect, 0), 0);
}

// ---------------------------------------------------------------------------
// Bar
// ---------------------------------------------------------------------------

TEST(DashboardPaintersTests, ABarIsFilledInProportionToItsValue)
{
    Scene scene;
    const DashboardWidget widget = barWidget();
    const QRectF track = barTrackRect(scene.rect);
    const double middle = track.center().y();

    scene.draw(widget, known(50.0), paintBar);

    EXPECT_TRUE(
        isColour(scene.image, track.left() + track.width() * 0.25, middle, scene.theme.accent));
    EXPECT_TRUE(
        isColour(scene.image, track.left() + track.width() * 0.75, middle, scene.theme.border));

    scene.draw(widget, known(100.0), paintBar);
    EXPECT_TRUE(
        isColour(scene.image, track.left() + track.width() * 0.75, middle, scene.theme.accent));

    scene.draw(widget, known(0.0), paintBar);
    EXPECT_TRUE(
        isColour(scene.image, track.left() + track.width() * 0.25, middle, scene.theme.border));

    // A value off the scale is pinned to it, which is how it is seen.
    scene.draw(widget, known(900.0), paintBar);
    EXPECT_TRUE(
        isColour(scene.image, track.left() + track.width() * 0.9, middle, scene.theme.accent));
}

TEST(DashboardPaintersTests, ABarWithNothingToShowIsAnEmptyGroove)
{
    Scene scene;
    const DashboardWidget widget = barWidget();
    const QRectF track = barTrackRect(scene.rect);

    scene.draw(widget, DashboardReading{}, paintBar);

    EXPECT_TRUE(isColour(
        scene.image, track.left() + track.width() * 0.25, track.center().y(), scene.theme.border));

    DashboardReading stale;
    stale.stale = true;
    scene.draw(widget, stale, paintBar);

    EXPECT_TRUE(isColour(
        scene.image, track.left() + track.width() * 0.25, track.center().y(), scene.theme.border));
}

TEST(DashboardPaintersTests, ABarPastALimitTakesTheColourOfThatLimit)
{
    Scene scene;
    DashboardWidget widget = barWidget();
    widget.limits.warningAbove = 70.0;
    widget.limits.criticalAbove = 90.0;

    const QRectF track = barTrackRect(scene.rect);
    const double at = track.left() + track.width() * 0.25;

    scene.draw(widget, known(50.0), paintBar);
    EXPECT_TRUE(isColour(scene.image, at, track.center().y(), scene.theme.accent));

    scene.draw(widget, known(80.0), paintBar);
    EXPECT_TRUE(isColour(scene.image, at, track.center().y(), scene.theme.lampAmber));

    scene.draw(widget, known(95.0), paintBar);
    EXPECT_TRUE(isColour(scene.image, at, track.center().y(), scene.theme.lampRed));
}

TEST(DashboardPaintersTests, ABarMarksWhereItsLimitsAre)
{
    Scene scene;
    DashboardWidget widget = barWidget();
    widget.limits.criticalAbove = 80.0;

    const QRectF track = barTrackRect(scene.rect);

    // At the height of the groove's edge, above it, where only the mark reaches.
    scene.draw(widget, known(10.0), paintBar);

    const double x = track.left() + track.width() * 0.8;
    EXPECT_TRUE(isColour(scene.image, x, track.top() - 1.0, scene.theme.lampRed, 40));

    // No limit, no mark.
    widget.limits = DashboardLimits{};
    scene.draw(widget, known(10.0), paintBar);
    EXPECT_FALSE(isColour(scene.image, x, track.top() - 1.0, scene.theme.lampRed, 40));
}

TEST(DashboardPaintersTests, AVerticalBarFillsFromTheBottom)
{
    Scene scene;
    scene.rect = QRectF{10.0, 10.0, 64.0, 220.0};
    scene.image = canvasFor(scene.theme, 100, 260);

    const DashboardWidget widget = barWidget();
    const QRectF track = barTrackRect(scene.rect);

    scene.theme = Theme::dark();

    QImage image{100, 260, QImage::Format_ARGB32_Premultiplied};
    image.fill(scene.theme.canvas);

    {
        QPainter painter{&image};
        const PaintContext context{scene.theme,
                                   widget,
                                   scene.rect,
                                   known(50.0),
                                   QStringLiteral("level"),
                                   scene.images,
                                   false};
        paintBar(painter, context);
    }

    EXPECT_TRUE(isColour(
        image, track.center().x(), track.bottom() - track.height() * 0.25, scene.theme.accent));
    EXPECT_TRUE(isColour(
        image, track.center().x(), track.top() + track.height() * 0.25, scene.theme.border));
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

TEST(DashboardPaintersTests, AStateIsTintedByWhatItsNameMeansAndAnUnnamedValueIsNot)
{
    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 180.0, 70.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::State);
    widget.id = "status";

    DashboardState fault;
    fault.value = 2.0;
    fault.label = "Fault";
    fault.tone = DashboardTone::Danger;

    DashboardState running;
    running.value = 1.0;
    running.label = "Running";
    running.tone = DashboardTone::Ok;

    DashboardState plain;
    plain.value = 0.0;
    plain.label = "Waiting";

    widget.states = {plain, running, fault};

    const QRectF chip = stateChipRect(scene.rect);

    // Well inside the chip, to the left of where the text is.
    const double x = chip.left() + 6.0;
    const double y = chip.center().y();

    // The widget is a card in the panel's colour, like a Numeric, with the chip a wash inside it.
    scene.draw(widget, known(2.0), paintState);
    EXPECT_TRUE(
        isColour(scene.image, x, y, blended(scene.theme.panel, scene.theme.lampRed, 34), 4));

    scene.draw(widget, known(1.0), paintState);
    EXPECT_TRUE(
        isColour(scene.image, x, y, blended(scene.theme.panel, scene.theme.success, 34), 4));

    // A name with no opinion is the quiet chip, and so is a number nobody named.
    scene.draw(widget, known(0.0), paintState);
    EXPECT_TRUE(isColour(scene.image, x, y, scene.theme.panelAlternate));

    scene.draw(widget, known(9.0), paintState);
    EXPECT_TRUE(isColour(scene.image, x, y, scene.theme.panelAlternate));

    // Nothing known is an outline and no fill: the card shows through.
    scene.draw(widget, DashboardReading{}, paintState);
    EXPECT_TRUE(isColour(scene.image, x, y, scene.theme.panel));

    // And the card is there: its own colour, outside the chip.
    EXPECT_TRUE(isColour(scene.image, 3.0, scene.rect.height() / 2.0, scene.theme.panel));
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

TEST(DashboardPaintersTests, AnImageDrawsThePictureItWasGiven)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    writePicture(QDir{directory.path()}, QStringLiteral("red.png"), QColor{255, 0, 0});

    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 120.0, 120.0};
    scene.images.setBaseDirectory(directory.path());

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Image);
    widget.id = "logo";
    widget.image = "red.png";

    scene.draw(widget, DashboardReading{}, paintImage);

    EXPECT_TRUE(isColour(scene.image, 60.0, 60.0, QColor{255, 0, 0}));

    // Fitted, not stretched: a square picture in a wide box leaves the box's ends alone.
    scene.rect = QRectF{0.0, 0.0, 240.0, 60.0};
    scene.draw(widget, DashboardReading{}, paintImage);

    EXPECT_TRUE(isColour(scene.image, 120.0, 30.0, QColor{255, 0, 0}));
    EXPECT_TRUE(isColour(scene.image, 10.0, 30.0, scene.theme.canvas));
}

TEST(DashboardPaintersTests, ABoundImageIsThePictureOfTheStateTheValueIsIn)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    writePicture(QDir{directory.path()}, QStringLiteral("red.png"), QColor{255, 0, 0});
    writePicture(QDir{directory.path()}, QStringLiteral("blue.png"), QColor{0, 0, 255});
    writePicture(QDir{directory.path()}, QStringLiteral("grey.png"), QColor{128, 128, 128});

    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 100.0, 100.0};
    scene.images.setBaseDirectory(directory.path());

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Image);
    widget.id = "door";
    widget.binding.source = DashboardBinding::Source::Variable;
    widget.binding.variable = "door";
    widget.image = "grey.png";

    DashboardState closed;
    closed.value = 0.0;
    closed.label = "Closed";
    closed.image = "red.png";

    DashboardState open;
    open.value = 1.0;
    open.label = "Open";
    open.image = "blue.png";

    widget.states = {closed, open};

    scene.draw(widget, known(0.0), paintImage);
    EXPECT_TRUE(isColour(scene.image, 50.0, 50.0, QColor{255, 0, 0}));

    scene.draw(widget, known(1.0), paintImage);
    EXPECT_TRUE(isColour(scene.image, 50.0, 50.0, QColor{0, 0, 255}));

    // A value no state names shows the widget's own picture.
    scene.draw(widget, known(7.0), paintImage);
    EXPECT_TRUE(isColour(scene.image, 50.0, 50.0, QColor{128, 128, 128}));

    // And one whose value is not known shows it turned down, because it is what it would be and not
    // what it is.
    scene.draw(widget, DashboardReading{}, paintImage);

    const QColor dimmed = scene.image.pixelColor(50, 50);
    EXPECT_LT(dimmed.red(), 128);
    EXPECT_GT(dimmed.red(), scene.theme.canvas.red());
}

TEST(DashboardPaintersTests,
     AnImageWithPicturesOnlyForItsStatesShowsTheFirstTurnedDownUntilItKnowsItsValue)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    writePicture(QDir{directory.path()}, QStringLiteral("red.png"), QColor{255, 0, 0});
    writePicture(QDir{directory.path()}, QStringLiteral("blue.png"), QColor{0, 0, 255});

    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 100.0, 100.0};
    scene.images.setBaseDirectory(directory.path());

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Image);
    widget.id = "regime";
    widget.binding.source = DashboardBinding::Source::Variable;
    widget.binding.variable = "regime";

    DashboardState idle;
    idle.value = 0.0;
    idle.image = "red.png";

    DashboardState busy;
    busy.value = 1.0;
    busy.image = "blue.png";

    widget.states = {idle, busy};

    // No value yet, no picture of its own: the first picture the states have, turned down - what
    // the widget would be - and not the second, which would say it was something else.
    scene.draw(widget, DashboardReading{}, paintImage);

    const QColor dimmed = scene.image.pixelColor(50, 50);
    EXPECT_GT(dimmed.red(), scene.theme.canvas.red());
    EXPECT_LT(dimmed.red(), 200);
    EXPECT_LT(dimmed.blue(), dimmed.red());

    // And in Edit mode it is not told to have a picture dropped on it, which it has.
    QImage edited = canvasFor(scene.theme, 100, 100);

    {
        QPainter painter{&edited};
        const PaintContext context{scene.theme,
                                   widget,
                                   scene.rect,
                                   DashboardReading{},
                                   QStringLiteral("caption"),
                                   scene.images,
                                   true};
        paintImage(painter, context);
    }

    EXPECT_EQ(countOf(edited, QRect{0, 0, 100, 100}, scene.theme.border, 8), 0);

    // A value that is known and that no state names, with no picture of its own, is in no state to
    // show: nothing, rather than a picture that says it is something else.
    scene.draw(widget, known(7.0), paintImage);
    EXPECT_TRUE(isColour(scene.image, 50.0, 50.0, scene.theme.canvas));
}

TEST(DashboardPaintersTests, APictureThatIsNotThereIsAPlaceholderAndNotAnEmptyHole)
{
    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 120.0, 120.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Image);
    widget.id = "gone";
    widget.image = "nowhere/gone.png";

    EXPECT_FALSE(scene.images.problemWith(widget.image).isEmpty());

    scene.draw(widget, DashboardReading{}, paintImage);

    // Something was drawn: the dashed outline of the placeholder.
    EXPECT_GT(countOf(scene.image, QRect{0, 0, 120, 120}, scene.theme.border, 8), 40);

    // An image with no picture at all draws nothing in Run mode and a prompt in Edit.
    widget.image.clear();
    scene.draw(widget, DashboardReading{}, paintImage);
    EXPECT_EQ(countOf(scene.image, QRect{0, 0, 120, 120}, scene.theme.border, 8), 0);
}

TEST(DashboardPaintersTests, ACachedPictureNoticesThatTheFileWasReplaced)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QDir dir{directory.path()};
    writePicture(dir, QStringLiteral("logo.png"), QColor{255, 0, 0});

    DashboardImages images;
    images.setBaseDirectory(directory.path());

    EXPECT_EQ(images.sizeOf("logo.png"), QSize(40, 40));
    EXPECT_TRUE(images.problemWith("logo.png").isEmpty());

    // The base directory is where a relative path goes; moving it forgets what was read.
    images.setBaseDirectory(directory.path() + QStringLiteral("/elsewhere"));
    EXPECT_FALSE(images.problemWith("logo.png").isEmpty());
    EXPECT_TRUE(images.sizeOf("logo.png").isEmpty());

    // Not a picture, though a file.
    {
        QFile file{dir.filePath(QStringLiteral("notes.png"))};
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("this is not a picture");
    }

    images.setBaseDirectory(directory.path());
    EXPECT_FALSE(images.problemWith("notes.png").isEmpty());
    EXPECT_FALSE(images.problemWith("missing.png").isEmpty());

    // Clearing makes the next look a fresh one.
    images.clear();
    EXPECT_EQ(images.sizeOf("logo.png"), QSize(40, 40));
}

TEST(DashboardPaintersTests, EveryPictureOfTheExampleProjectsLoadsAndHasASize)
{
    // The examples are what a new user opens first, and a picture that does not load is a box that
    // says so on the first screen. The files are checked to exist elsewhere; this checks that what
    // is there is a picture this build can read.
    for (const char* file : {"j1939-vehicle.tbsproj", "virtual-vehicle.tbsproj"}) {
        SCOPED_TRACE(file);

        const QString path =
            QStringLiteral(TORQUEBUS_EXAMPLE_PROJECT_DIR "/") + QString::fromLatin1(file);

        GraphDescription pipeline;
        TransmitList transmit;
        DashboardDescription dashboard;
        ASSERT_TRUE(services::ProjectFile::load(path, pipeline, transmit, dashboard).succeeded());

        DashboardImages images;
        images.setBaseDirectory(QFileInfo{path}.absolutePath());

        const std::vector<std::string> used = assetsUsedBy(dashboard);
        ASSERT_FALSE(used.empty());

        for (const std::string& image : used) {
            SCOPED_TRACE(image);

            EXPECT_TRUE(images.problemWith(image).isEmpty())
                << images.problemWith(image).toStdString();
            EXPECT_FALSE(images.sizeOf(image).isEmpty());
        }
    }
}

// ---------------------------------------------------------------------------
// Trend
// ---------------------------------------------------------------------------

namespace {

/// A rising line across the whole of a thirty second window, `count` samples of it.
[[nodiscard]] SeriesWindow rampWindow(std::size_t count, double peak)
{
    SeriesWindow window;

    for (std::size_t i = 0; i < count; ++i) {
        SignalSample sample;
        sample.timestampNs = 30'000'000'000ULL * i / (count - 1);
        sample.value = peak * static_cast<double>(i) / static_cast<double>(count - 1);
        window.samples.push_back(sample);
    }

    return window;
}

} // namespace

TEST(DashboardPaintersTests, ATrendDrawsItsLineAcrossItsWindow)
{
    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 260.0, 140.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Trend);
    widget.id = "rpm";
    widget.minimum = 0.0;
    widget.maximum = 100.0;
    widget.windowSeconds = 30.0;

    const QRectF plot = trendPlotRect(scene.rect);
    const SeriesWindow window = rampWindow(60, 100.0);

    scene.draw(widget, known(100.0), [&](QPainter& painter, const PaintContext& context) {
        paintTrend(painter, context, &window, 30'000'000'000ULL);
    });

    // The line climbs from the lower left to the upper right of the plot.
    const QRect lowerLeft{
        static_cast<int>(plot.left()), static_cast<int>(plot.bottom() - 25), 25, 25};
    const QRect upperLeft{static_cast<int>(plot.left()), static_cast<int>(plot.top()), 25, 25};
    const QRect upperRight{
        static_cast<int>(plot.right() - 25), static_cast<int>(plot.top()), 25, 25};

    EXPECT_GT(countOf(scene.image, lowerLeft, scene.theme.accent), 5);
    EXPECT_EQ(countOf(scene.image, upperLeft, scene.theme.accent), 0);
    EXPECT_GT(countOf(scene.image, upperRight, scene.theme.accent), 5);
}

TEST(DashboardPaintersTests, ATrendWithNothingToDrawSaysSoAndDrawsNoLine)
{
    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 260.0, 140.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Trend);
    widget.id = "rpm";

    scene.draw(widget, DashboardReading{}, [](QPainter& painter, const PaintContext& context) {
        paintTrend(painter, context, nullptr, 0);
    });

    EXPECT_EQ(countOf(scene.image, QRect{0, 0, 260, 140}, scene.theme.accent), 0);

    // An empty window is the same as none.
    const SeriesWindow empty;
    scene.draw(widget, DashboardReading{}, [&](QPainter& painter, const PaintContext& context) {
        paintTrend(painter, context, &empty, 1'000'000'000ULL);
    });

    EXPECT_EQ(countOf(scene.image, QRect{0, 0, 260, 140}, scene.theme.accent), 0);
}

TEST(DashboardPaintersTests, ATrendBreaksItsLineWhereASampleIsNotANumber)
{
    Scene scene;
    scene.rect = QRectF{0.0, 0.0, 260.0, 140.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Trend);
    widget.id = "rpm";
    widget.minimum = 0.0;
    widget.maximum = 100.0;
    widget.windowSeconds = 10.0;

    // A line is exactly the colour of its pen only where it covers a whole pixel, so the level is
    // the middle of a row of pixels - and well away from the lines of the scale, which are drawn at
    // the quarters.
    const QRectF plot = trendPlotRect(scene.rect);
    const double y = std::floor(plot.top() + plot.height() * 0.4) + 0.5;
    const double level = (plot.bottom() - y) / plot.height() * 100.0;

    // Eleven samples a second apart, flat, with the sixth "not available".
    SeriesWindow window;

    for (int i = 0; i <= 10; ++i) {
        SignalSample sample;
        sample.timestampNs = static_cast<std::uint64_t>(i) * 1'000'000'000ULL;
        sample.value = i == 5 ? std::numeric_limits<double>::quiet_NaN() : level;
        window.samples.push_back(sample);
    }

    scene.draw(widget, known(level), [&](QPainter& painter, const PaintContext& context) {
        paintTrend(painter, context, &window, 10'000'000'000ULL);
    });

    // Before the gap and after it there is line; in the middle of it there is none.
    const auto columnHas = [&](double fraction) {
        const int x = static_cast<int>(plot.left() + plot.width() * fraction);
        return countOf(scene.image, QRect{x - 1, static_cast<int>(y) - 3, 3, 7}, scene.theme.accent)
               > 0;
    };

    EXPECT_TRUE(columnHas(0.2));
    EXPECT_TRUE(columnHas(0.8));
    EXPECT_FALSE(columnHas(0.5));
}

// ---------------------------------------------------------------------------
// Table, group, badge
// ---------------------------------------------------------------------------

TEST(DashboardPaintersTests, ATableDrawsWhateverItIsGivenWithoutOverrunningItsBox)
{
    Scene scene;
    scene.rect = QRectF{20.0, 20.0, 220.0, 150.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Table);
    widget.id = "eec1";

    std::vector<TableRow> rows;

    for (int i = 0; i < 40; ++i) {
        TableRow row;
        row.name = QStringLiteral("Signal number %1").arg(i);
        row.value = QStringLiteral("%1 rpm").arg(i * 100);
        row.known = i % 2 == 0;
        row.tone = i % 5 == 0 ? DashboardTone::Danger : DashboardTone::Neutral;
        rows.push_back(row);
    }

    scene.draw(widget, DashboardReading{}, [&](QPainter& painter, const PaintContext& context) {
        paintTable(painter, context, rows, QStringLiteral("nothing"));
    });

    // Nothing is drawn outside the box it was given.
    const QImage blank = canvasFor(scene.theme);
    bool outside = false;

    for (int y = 0; y < scene.image.height() && !outside; ++y) {
        for (int x = 0; x < scene.image.width(); ++x) {
            const bool inside = scene.rect.adjusted(-1.0, -1.0, 1.0, 1.0).contains(x, y);

            if (!inside && scene.image.pixel(x, y) != blank.pixel(x, y)) {
                outside = true;
                break;
            }
        }
    }

    EXPECT_FALSE(outside);

    // And with no rows it says what to do instead.
    scene.draw(widget, DashboardReading{}, [](QPainter& painter, const PaintContext& context) {
        paintTable(painter, context, {}, QStringLiteral("Pick a message."));
    });

    EXPECT_GT(countOf(scene.image, scene.rect.toRect(), scene.theme.textMuted, 40), 0);
}

TEST(DashboardPaintersTests, AGroupIsAFrameWithAWashAndNothingElse)
{
    Scene scene;
    scene.rect = QRectF{10.0, 10.0, 200.0, 140.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Group);
    widget.id = "box";

    scene.draw(widget, DashboardReading{}, paintGroup);

    // Inside it is the canvas with the panel's colour faintly over it, and so not the canvas.
    const QColor inside = scene.image.pixelColor(110, 100);
    EXPECT_NE(inside, scene.theme.canvas);
    EXPECT_NEAR(inside.red(), blended(scene.theme.canvas, scene.theme.panel, 110).red(), 3);

    // Outside it is untouched.
    EXPECT_TRUE(isColour(scene.image, 3.0, 3.0, scene.theme.canvas, 0));
}

TEST(DashboardPaintersTests, AStaleBadgeIsDrawnInTheCornerOfAWidgetAndNowhereElse)
{
    Scene scene;
    scene.rect = QRectF{20.0, 20.0, 160.0, 100.0};

    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Numeric);
    widget.id = "speed";

    scene.draw(widget, DashboardReading{}, paintStaleBadge);

    EXPECT_GT(countOf(scene.image, QRect{120, 20, 62, 16}, scene.theme.lampAmber, 60), 0);
    EXPECT_EQ(countOf(scene.image, QRect{20, 60, 100, 50}, scene.theme.lampAmber, 60), 0);
}

// ---------------------------------------------------------------------------
// Faults
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] FaultRow faultRow(const char* code, DashboardTone tone)
{
    FaultRow row;
    row.code = QString::fromLatin1(code);
    row.meaning =
        QStringLiteral("Data valid but below normal operational range - most severe level");
    row.count = QStringLiteral("×1");
    row.ecu = QStringLiteral("SA 0");
    row.tone = tone;
    return row;
}

[[nodiscard]] DashboardWidget faultsWidget()
{
    DashboardWidget widget = defaultWidgetOf(DashboardWidgetKind::Faults);
    widget.id = "faults";
    return widget;
}

/// Draws a fault list in a box of the size a person gives one, on a canvas with room around it.
void drawFaults(Scene& scene,
                const FaultLamps& lamps,
                const std::vector<FaultRow>& rows,
                const QRectF& rect = QRectF{20.0, 10.0, 320.0, 260.0})
{
    scene.size = QSize{static_cast<int>(rect.right()) + 20, static_cast<int>(rect.bottom()) + 20};
    scene.rect = rect;

    scene.draw(
        faultsWidget(), DashboardReading{}, [&](QPainter& painter, const PaintContext& context) {
            paintFaults(painter, context, lamps, rows);
        });
}

/// A point on the bar at the left edge of row `index`, which is as wide as the tint is strong.
[[nodiscard]] QPointF barOf(const QRectF& rect, int index)
{
    const QRectF line = faultRowRect(rect, index);

    return QPointF{line.left() + 3.5, line.center().y()};
}

/// A point of row `index` where nothing but its wash is drawn: the corner below the text, past the
/// end of the ECU's name.
[[nodiscard]] QPointF washOf(const QRectF& rect, int index)
{
    const QRectF line = faultRowRect(rect, index);

    return QPointF{line.right() - 4.0, line.bottom() - 3.0};
}

} // namespace

TEST(DashboardPaintersTests, AFaultListLightsTheLampsOfTheBusInFullAndLeavesTheOthersDark)
{
    Scene scene;

    FaultLamps lamps;
    lamps.stop = true;
    lamps.protect = true;

    drawFaults(scene, lamps, {});

    const auto centre = [&](int lamp) { return faultLampRect(scene.rect, lamp).center(); };

    // Stop is red and the others are the amber of an attention lamp, lit all the way through.
    EXPECT_TRUE(isColour(scene.image, centre(0).x(), centre(0).y(), scene.theme.lampRed, 1));
    EXPECT_TRUE(isColour(scene.image, centre(3).x(), centre(3).y(), scene.theme.lampAmber, 1));

    // A lamp that is off is still there: the outline and a faint wash of its own colour, and not
    // the colour. It has to look like the lamp that was on.
    const QColor dimRed = blended(scene.theme.panel, scene.theme.lampRed, 40);
    const QColor dimAmber = blended(scene.theme.panel, scene.theme.lampAmber, 40);

    EXPECT_TRUE(isColour(scene.image, centre(1).x(), centre(1).y(), dimAmber, 3));
    EXPECT_TRUE(isColour(scene.image, centre(2).x(), centre(2).y(), dimAmber, 3));
    EXPECT_FALSE(isColour(scene.image, centre(1).x(), centre(1).y(), scene.theme.lampAmber, 20));

    // And with the other two lit instead, the picture is the opposite one.
    FaultLamps other;
    other.warning = true;
    other.malfunction = true;

    drawFaults(scene, other, {});

    EXPECT_TRUE(isColour(scene.image, centre(1).x(), centre(1).y(), scene.theme.lampAmber, 1));
    EXPECT_TRUE(isColour(scene.image, centre(2).x(), centre(2).y(), scene.theme.lampAmber, 1));
    EXPECT_TRUE(isColour(scene.image, centre(0).x(), centre(0).y(), dimRed, 3));
    EXPECT_TRUE(isColour(scene.image, centre(3).x(), centre(3).y(), dimAmber, 3));
}

TEST(DashboardPaintersTests, AFaultListWithNothingToListSaysSoAndIsNotAnError)
{
    Scene scene;

    // A healthy bus, a measurement that is not running and a graph with no J1939 block are all this
    // picture: dark lamps and an empty list.
    drawFaults(scene, FaultLamps{}, {});

    const QRect list = faultListRect(scene.rect).toRect();

    // Words, in the quiet colour; and not one pixel of the colours of alarm anywhere in the list.
    EXPECT_GT(countOf(scene.image, list, scene.theme.textMuted, 40), 0);
    EXPECT_EQ(countOf(scene.image, list, scene.theme.lampRed, 30), 0);
    EXPECT_EQ(countOf(scene.image, list, scene.theme.lampAmber, 30), 0);
    EXPECT_EQ(countOf(scene.image, QRect{20, 10, 320, 60}, scene.theme.lampRed, 4), 0);

    // The same when a lamp is lit and there is no code to go with it: a lamp is not a row.
    FaultLamps lit;
    lit.stop = true;

    drawFaults(scene, lit, {});

    EXPECT_EQ(countOf(scene.image, list, scene.theme.lampRed, 4), 0);
    EXPECT_GT(countOf(scene.image, list, scene.theme.textMuted, 40), 0);
}

TEST(DashboardPaintersTests, ARowIsTintedByTheLampThatGoesWithItAndOnlyByThat)
{
    Scene scene;

    const std::vector<FaultRow> rows{faultRow("SPN 100 / FMI 1", DashboardTone::Danger),
                                     faultRow("SPN 110 / FMI 0", DashboardTone::Warning),
                                     faultRow("SPN 168 / FMI 4", DashboardTone::Neutral)};

    drawFaults(scene, FaultLamps{}, rows);

    // The bar at the edge: the colour of the lamp in full, and the quiet outline colour for a code
    // whose message lit no lamp.
    const auto bar = [&](int row) { return barOf(scene.rect, row); };

    EXPECT_TRUE(isColour(scene.image, bar(0).x(), bar(0).y(), scene.theme.lampRed, 2));
    EXPECT_TRUE(isColour(scene.image, bar(1).x(), bar(1).y(), scene.theme.lampAmber, 2));
    EXPECT_TRUE(isColour(scene.image, bar(2).x(), bar(2).y(), scene.theme.border, 2));

    // And a faint wash over the row, so that a tinted row reads as one from across a room.
    const auto wash = [&](int row) { return washOf(scene.rect, row); };

    EXPECT_TRUE(isColour(scene.image,
                         wash(0).x(),
                         wash(0).y(),
                         blended(scene.theme.panel, scene.theme.lampRed, 34),
                         3));
    EXPECT_TRUE(isColour(scene.image,
                         wash(1).x(),
                         wash(1).y(),
                         blended(scene.theme.panel, scene.theme.lampAmber, 34),
                         3));

    // The one with no opinion is the panel itself: the first row of a plain list is not striped.
    EXPECT_TRUE(isColour(scene.image, wash(2).x(), wash(2).y(), scene.theme.panel, 2));

    // Each row's code is in its colour too, in the text: the first line of a red row has red in it
    // and the first line of a plain one has none.
    const auto firstLine = [&](int row) {
        const QRectF line = faultRowRect(scene.rect, row);
        return QRect{static_cast<int>(line.left()) + 12, static_cast<int>(line.top()) + 3, 120, 18};
    };

    EXPECT_GT(countOf(scene.image, firstLine(0), scene.theme.lampRed, 40), 0);
    EXPECT_GT(countOf(scene.image, firstLine(1), scene.theme.lampAmber, 40), 0);
    EXPECT_EQ(countOf(scene.image, firstLine(2), scene.theme.lampRed, 40), 0);
    EXPECT_EQ(countOf(scene.image, firstLine(2), scene.theme.lampAmber, 40), 0);
}

TEST(DashboardPaintersTests, MoreRowsThanFitShowOnlyWhatFitsAndSayHowManyAreLeft)
{
    Scene scene;
    const QRectF rect{20.0, 10.0, 320.0, 260.0};

    // Room for four rows: the list is 197 pixels high and a row is 40.
    EXPECT_EQ(faultRowCapacity(rect, 0), 0);
    EXPECT_EQ(faultRowCapacity(rect, 3), 3);
    EXPECT_EQ(faultRowCapacity(rect, 4), 4);

    // A fifth does not fit - and it is not given the place of the line that says so.
    EXPECT_EQ(faultRowCapacity(rect, 5), 4);
    EXPECT_EQ(faultRowCapacity(rect, 100), 4);

    // The line takes some of the room: where three rows fit exactly, four leave two and the line.
    EXPECT_EQ(faultRowCapacity(QRectF{0.0, 0.0, 320.0, 183.0}, 3), 3);
    EXPECT_EQ(faultRowCapacity(QRectF{0.0, 0.0, 320.0, 183.0}, 4), 2);

    // A box too short for a row shows none, and does not go negative.
    EXPECT_EQ(faultRowCapacity(QRectF{0.0, 0.0, 320.0, 70.0}, 3), 0);

    // Ten rows, all red: four are drawn, and under the fourth is the line that counts the rest.
    std::vector<FaultRow> rows;

    for (int i = 0; i < 10; ++i) {
        rows.push_back(faultRow("SPN 100 / FMI 1", DashboardTone::Danger));
    }

    drawFaults(scene, FaultLamps{}, rows, rect);

    for (int row = 0; row < 4; ++row) {
        const QPointF bar = barOf(rect, row);
        EXPECT_TRUE(isColour(scene.image, bar.x(), bar.y(), scene.theme.lampRed, 2)) << row;
    }

    // The fifth place is not a row: no bar there, and no wash. A line of words is.
    const QPointF fifth = barOf(rect, 4);
    EXPECT_FALSE(isColour(scene.image, fifth.x(), fifth.y(), scene.theme.lampRed, 30));

    const QRectF moreLine{
        rect.left() + 8.0, faultRowRect(rect, 4).top(), rect.width() - 16.0, kFaultsMoreHeight};

    EXPECT_GT(countOf(scene.image, moreLine.toRect(), scene.theme.textMuted, 40), 0);

    // With exactly as many rows as fit there is no such line: the place is blank.
    rows.resize(4);
    drawFaults(scene, FaultLamps{}, rows, rect);

    EXPECT_EQ(countOf(scene.image, moreLine.toRect(), scene.theme.textMuted, 40), 0);
}

TEST(DashboardPaintersTests, NothingOfAFaultListIsDrawnOutsideItsBoxHoweverManyRowsOrHowSmall)
{
    Scene scene;

    std::vector<FaultRow> rows;

    for (int i = 0; i < 40; ++i) {
        FaultRow row = faultRow("SPN 524287 / FMI 31",
                                i % 3 == 0 ? DashboardTone::Danger : DashboardTone::Warning);
        row.meaning = QStringLiteral("A reason so long that it cannot possibly fit in any box a "
                                     "person would make for this widget, however wide it is");
        row.ecu = QStringLiteral("SA 253 · An ECU with a long name indeed");
        rows.push_back(row);
    }

    FaultLamps lamps;
    lamps.stop = lamps.warning = lamps.malfunction = lamps.protect = true;

    for (const QRectF rect : {QRectF{20.0, 10.0, 320.0, 260.0},
                              QRectF{20.0, 10.0, 120.0, 90.0},
                              QRectF{20.0, 10.0, 40.0, 30.0},
                              QRectF{20.0, 10.0, 700.0, 70.0}}) {
        SCOPED_TRACE(::testing::Message() << rect.width() << " x " << rect.height());

        drawFaults(scene, lamps, rows, rect);

        const QImage blank = canvasFor(scene.theme, scene.image.width(), scene.image.height());
        bool outside = false;

        for (int y = 0; y < scene.image.height() && !outside; ++y) {
            for (int x = 0; x < scene.image.width(); ++x) {
                const bool inside = rect.adjusted(-1.0, -1.0, 1.0, 1.0).contains(x, y);

                if (!inside && scene.image.pixel(x, y) != blank.pixel(x, y)) {
                    outside = true;
                    break;
                }
            }
        }

        EXPECT_FALSE(outside);
    }
}

TEST(DashboardPaintersTests, TheLampsOfAFaultListFitInItAndApartAtEveryWidthAPersonWouldMakeIt)
{
    for (const double width : {120.0, 160.0, 200.0, 248.0, 320.0, 720.0, 980.0}) {
        SCOPED_TRACE(::testing::Message() << "width " << width);

        const QRectF rect{10.0, 10.0, width, 200.0};
        QRectF previous;

        for (int lamp = 0; lamp < 4; ++lamp) {
            const QRectF bulb = faultLampRect(rect, lamp);

            EXPECT_TRUE(rect.contains(bulb)) << "lamp " << lamp;
            EXPECT_FALSE(bulb.intersects(previous)) << "lamp " << lamp;

            // In a line, above the list.
            EXPECT_LT(bulb.bottom(), faultListRect(rect).top());

            previous = bulb;
        }
    }
}

TEST(DashboardPaintersTests, AFaultListReadsTheSameInTheLightTheme)
{
    // Colour comes from the theme and nowhere else: the same widget on the light theme is the
    // light theme's red and amber, and its panel.
    Scene scene;
    scene.theme = Theme::light();

    FaultLamps lamps;
    lamps.stop = true;

    const std::vector<FaultRow> rows{faultRow("SPN 100 / FMI 1", DashboardTone::Danger)};

    drawFaults(scene, lamps, rows);

    const QPointF lamp = faultLampRect(scene.rect, 0).center();
    const QPointF bar = barOf(scene.rect, 0);

    EXPECT_TRUE(isColour(scene.image, lamp.x(), lamp.y(), scene.theme.lampRed, 1));
    EXPECT_TRUE(isColour(scene.image, bar.x(), bar.y(), scene.theme.lampRed, 2));
}
