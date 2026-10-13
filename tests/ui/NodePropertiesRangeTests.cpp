// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The form for a block's settings, and the range each setting says it has.
//
// The catalog states, for a number, the range it may take, and refuses a value outside it
// while the block is on screen. The form is where most of those values are typed, so the
// best place to refuse one is the box it is typed into: a channel that stops at 255 cannot
// be entered as 300 and read as something else. And because a project edited by hand can
// hold a value outside the range, the box has to show what is stored and not what would be
// allowed - a spin box that said 255 over a stored 300 would be telling the person what the
// block does not have.

#include "core/pipeline/GraphDescription.h"
#include "core/pipeline/NodeCatalog.h"
#include "ui/properties/NodePropertiesEditor.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QSpinBox>

using namespace torquebus;
using namespace torquebus::ui;

namespace {

/// A spin box in the form by the text on its label.
template<typename SpinBox>
SpinBox* spinBoxes(NodePropertiesEditor& editor, int index)
{
    const auto all = editor.findChildren<SpinBox*>();
    return index < all.size() ? all.at(index) : nullptr;
}

} // namespace

TEST(NodePropertiesRangeTest, ANumberIsEnteredWithinTheRangeTheBlockDeclares)
{
    GraphDescription description;
    description.addNode(NodeDescription{.id = "can_1",
                                        .typeName = "can.source",
                                        .parameters = {{"channel", ParameterValue::fromInteger(2)}},
                                        .x = 0.0,
                                        .y = 0.0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    NodePropertiesEditor editor{description, catalog};
    editor.showNode(QStringLiteral("can_1"));

    auto* channel = spinBoxes<QSpinBox>(editor, 0);
    ASSERT_NE(channel, nullptr);

    EXPECT_EQ(channel->value(), 2);
    EXPECT_EQ(channel->minimum(), 0);
    EXPECT_EQ(channel->maximum(), 255) << "a channel is one byte";

    // Typed past the end of it, the box takes the largest.
    channel->setValue(300);
    EXPECT_EQ(channel->value(), 255);
    EXPECT_EQ(description.find("can_1")->parameters.integer("channel", -1), 255);
}

TEST(NodePropertiesRangeTest, AStoredValueOutsideTheRangeIsShownAsItIs)
{
    // From a project edited by hand. The Output panel says it is out of range; the box has to
    // say what the block holds, so that it can be seen and corrected.
    GraphDescription description;
    description.addNode(
        NodeDescription{.id = "can_1",
                        .typeName = "can.source",
                        .parameters = {{"channel", ParameterValue::fromInteger(300)}},
                        .x = 0.0,
                        .y = 0.0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    NodePropertiesEditor editor{description, catalog};
    editor.showNode(QStringLiteral("can_1"));

    auto* channel = spinBoxes<QSpinBox>(editor, 0);
    ASSERT_NE(channel, nullptr);

    EXPECT_EQ(channel->value(), 300);
    EXPECT_GE(channel->maximum(), 300);
    EXPECT_TRUE(description.validate(catalog).failed());
}

TEST(NodePropertiesRangeTest, ABlockWithNoRangeKeepsTheWholeOfAnInt)
{
    // A CAN identifier does not fit a default 0 to 99, and a box that clamped
    // 0x18FEE500 to it would be worse than no box.
    GraphDescription description;
    description.addNode(
        NodeDescription{.id = "ml",
                        .typeName = "tinyml.ecu",
                        .parameters = {{"speedCanId", ParameterValue::fromInteger(0x18FEE500)}},
                        .x = 0.0,
                        .y = 0.0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    NodePropertiesEditor editor{description, catalog};
    editor.showNode(QStringLiteral("ml"));

    bool found = false;
    for (QSpinBox* spin : editor.findChildren<QSpinBox*>()) {
        if (spin->value() == 0x18FEE500) {
            found = true;
            EXPECT_GE(spin->maximum(), 0x18FEE500);
        }
    }

    EXPECT_TRUE(found) << "the identifier was not shown as it is stored";
}

TEST(NodePropertiesRangeTest, ARealNumberKeepsItsRangeToo)
{
    GraphDescription description;
    description.addNode(
        NodeDescription{.id = "replay",
                        .typeName = "log.source",
                        .parameters = {{"path", ParameterValue::fromText("x.tblog")},
                                       {"speed", ParameterValue::fromReal(2.5)}},
                        .x = 0.0,
                        .y = 0.0});

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    NodePropertiesEditor editor{description, catalog};
    editor.showNode(QStringLiteral("replay"));

    auto* speed = spinBoxes<QDoubleSpinBox>(editor, 0);
    ASSERT_NE(speed, nullptr);

    EXPECT_DOUBLE_EQ(speed->value(), 2.5);
    EXPECT_DOUBLE_EQ(speed->minimum(), 0.001);
    EXPECT_DOUBLE_EQ(speed->maximum(), 1000.0);
}
