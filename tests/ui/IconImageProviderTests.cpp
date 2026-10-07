// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The provider is what stands between a QML Image and the icon set, and a mistake in it
// shows up as a blank lamp on a screen nobody is looking at. These tests are the look.

#include "ui/theme/IconImageProvider.h"

#include <QColor>
#include <QDirIterator>
#include <QImage>
#include <QSize>

#include <gtest/gtest.h>

#include <algorithm>

namespace torquebus::ui {
namespace {

QImage request(const QString& id, const QSize& wanted, QSize* delivered = nullptr)
{
    IconImageProvider provider;
    return provider.requestImage(id, delivered, wanted);
}

bool hasInk(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                return true;
            }
        }
    }
    return false;
}

/// Ink on the outermost ring of pixels: an icon that touches the edge of its box has
/// been drawn outside the 24 x 24 grid it was meant for, and is clipped on screen.
bool inksTheEdge(const QImage& image)
{
    for (int i = 0; i < image.width(); ++i) {
        if (qAlpha(image.pixel(i, 0)) > 0 || qAlpha(image.pixel(i, image.height() - 1)) > 0) {
            return true;
        }
    }
    for (int i = 0; i < image.height(); ++i) {
        if (qAlpha(image.pixel(0, i)) > 0 || qAlpha(image.pixel(image.width() - 1, i)) > 0) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(IconImageProviderTests, ThePictureIsInTheColourAskedForAndAtTheSizeAskedFor)
{
    // The whole point: one white icon in the set, any colour out of it.
    QSize delivered;
    const QImage image =
        request(QStringLiteral("cluster-hazard/ff0000"), QSize{48, 48}, &delivered);

    ASSERT_FALSE(image.isNull());
    ASSERT_TRUE(delivered == QSize(48, 48));
    ASSERT_TRUE(image.size() == QSize(48, 48));
    ASSERT_TRUE(hasInk(image));

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() < 128) {
                continue;
            }
            ASSERT_TRUE(pixel.red() >= 250);
            ASSERT_TRUE(pixel.green() <= 5);
            ASSERT_TRUE(pixel.blue() <= 5);
        }
    }
}

TEST(IconImageProviderTests, AnAlphaInTheColourIsKept)
{
    // A lamp's halo is the lamp's colour at a fraction of its strength, and a colour
    // that arrives as AARRGGBB has to stay that translucent all the way to the pixels.
    const QImage image = request(QStringLiteral("cluster-hazard/80ffffff"), QSize{48, 48});

    ASSERT_FALSE(image.isNull());

    int strongest = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            strongest = std::max(strongest, qAlpha(image.pixel(x, y)));
        }
    }
    ASSERT_TRUE(strongest > 100);
    ASSERT_TRUE(strongest <= 130);
}

TEST(IconImageProviderTests, WithoutASizeTheIconKeepsItsOwn)
{
    QSize delivered;
    const QImage image = request(QStringLiteral("cluster-hazard/ffffff"), QSize{}, &delivered);

    ASSERT_FALSE(image.isNull());
    ASSERT_TRUE(delivered == QSize(24, 24));
}

TEST(IconImageProviderTests, ABoxOfAnotherShapeDoesNotStretchTheIcon)
{
    // The turn arrow is wider than it is tall. Asked for a square it must sit inside it
    // with its proportions, so the top and bottom of the box stay empty.
    const QImage image = request(QStringLiteral("cluster-turn-arrow/ffffff"), QSize{86, 86});

    ASSERT_FALSE(image.isNull());
    ASSERT_TRUE(hasInk(image));

    for (int x = 0; x < image.width(); ++x) {
        ASSERT_TRUE(qAlpha(image.pixel(x, 0)) == 0);
        ASSERT_TRUE(qAlpha(image.pixel(x, image.height() - 1)) == 0);
    }
}

TEST(IconImageProviderTests, AnAddressThatIsNotAnIconIsRefusedNotResolved)
{
    // Nothing in a binding should be able to reach a file that is not in the icon set,
    // and a typo should be an empty image the Image reports, not an exception.
    const QSize size{24, 24};

    ASSERT_TRUE(request(QStringLiteral("no-such-icon/ffffff"), size).isNull());
    ASSERT_TRUE(request(QStringLiteral("cluster-hazard"), size).isNull());
    ASSERT_TRUE(request(QStringLiteral("cluster-hazard/"), size).isNull());
    ASSERT_TRUE(request(QStringLiteral("/ffffff"), size).isNull());
    ASSERT_TRUE(request(QString{}, size).isNull());

    // a colour is hex digits, never a name
    ASSERT_TRUE(request(QStringLiteral("cluster-hazard/red"), size).isNull());
    ASSERT_TRUE(request(QStringLiteral("cluster-hazard/ff00"), size).isNull());

    // a name is a file in resources/icons and nothing that leaves it
    ASSERT_TRUE(request(QStringLiteral("../themes/torquebus/ffffff"), size).isNull());
    ASSERT_TRUE(request(QStringLiteral("a/b/ffffff"), size).isNull());
    ASSERT_TRUE(request(QStringLiteral("Telltale-Hazard/ffffff"), size).isNull());
}

TEST(IconImageProviderTests, EveryTelltaleIconDrawsSomethingInsideItsBox)
{
    // The drawings are path data typed by hand; one wrong digit gives a blank lamp or a
    // clipped one. This finds the icon, not the screen it was supposed to appear on.
    int found = 0;

    QDirIterator it{QStringLiteral(":/icons"), {QStringLiteral("cluster-*.svg")}, QDir::Files};
    while (it.hasNext()) {
        it.next();
        ++found;

        const QString name = it.fileInfo().completeBaseName();
        SCOPED_TRACE(name.toStdString());

        const QImage image = request(name + QStringLiteral("/ffffff"), QSize{96, 96});
        ASSERT_FALSE(image.isNull());
        ASSERT_TRUE(hasInk(image));
        ASSERT_FALSE(inksTheEdge(image));
    }

    ASSERT_TRUE(found >= 15);
}

} // namespace torquebus::ui
