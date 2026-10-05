// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/GraphDescription.h"
#include "ui/scripting/ScriptEdit.h"
#include "ui/scripting/ScriptEditorPanel.h"

#include <QApplication>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <gtest/gtest.h>

namespace torquebus::ui {
namespace {

TEST(ScriptEditTests, CommentToggleAndIndentation)
{
    ScriptEdit editor;
    editor.setPlainText(QStringLiteral("local a = 1\nlocal b = 2\nlocal c = 3"));

    // Select all lines
    editor.selectAll();

    // Toggle comment -> all lines should be commented with "-- "
    editor.toggleComment();
    EXPECT_TRUE(editor.toPlainText().contains(QStringLiteral("-- local a = 1")));
    EXPECT_TRUE(editor.toPlainText().contains(QStringLiteral("-- local b = 2")));
    EXPECT_TRUE(editor.toPlainText().contains(QStringLiteral("-- local c = 3")));

    // Toggle comment again -> all comments should be stripped
    editor.selectAll();
    editor.toggleComment();
    EXPECT_EQ(editor.toPlainText(), QStringLiteral("local a = 1\nlocal b = 2\nlocal c = 3"));

    // Test Indent / Unindent
    editor.selectAll();
    editor.indentSelection();
    EXPECT_TRUE(editor.toPlainText().startsWith(QStringLiteral("    local a = 1")));

    editor.selectAll();
    editor.unindentSelection();
    EXPECT_EQ(editor.toPlainText(), QStringLiteral("local a = 1\nlocal b = 2\nlocal c = 3"));
}

TEST(ScriptEditTests, ZoomControls)
{
    ScriptEdit editor;
    EXPECT_DOUBLE_EQ(editor.pointSize(), 10.0);

    editor.zoomIn(2.0);
    EXPECT_DOUBLE_EQ(editor.pointSize(), 12.0);

    editor.zoomOut(1.0);
    EXPECT_DOUBLE_EQ(editor.pointSize(), 11.0);

    editor.resetZoom();
    EXPECT_DOUBLE_EQ(editor.pointSize(), 10.0);
}

TEST(ScriptEditTests, FindAndReplace)
{
    ScriptEdit editor;
    editor.setPlainText(QStringLiteral("foo = 10\nbar = foo + 5\nfoo = foo * 2"));

    // Find forward
    EXPECT_TRUE(editor.findText(QStringLiteral("foo"), true, false, false));
    EXPECT_EQ(editor.textCursor().selectedText(), QStringLiteral("foo"));

    // Replace current
    EXPECT_TRUE(editor.replaceCurrent(QStringLiteral("foo"), QStringLiteral("speed"), true, false));
    EXPECT_TRUE(editor.toPlainText().startsWith(QStringLiteral("speed = 10")));

    // Replace all remaining occurrences
    const int count =
        editor.replaceAll(QStringLiteral("foo"), QStringLiteral("speed"), true, false);
    EXPECT_EQ(count, 3);
    EXPECT_FALSE(editor.toPlainText().contains(QStringLiteral("foo")));
}

TEST(ScriptEditTests, DocumentMetrics)
{
    ScriptEdit editor;
    editor.setPlainText(QStringLiteral("first line\nsecond line\nthird"));

    EXPECT_EQ(editor.lineCount(), 3);
    EXPECT_EQ(editor.characterCount(), 28);

    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down);
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, 4);
    editor.setTextCursor(cursor);

    EXPECT_EQ(editor.currentLine(), 2);
    EXPECT_EQ(editor.currentColumn(), 5);
}

TEST(ScriptEditorPanelTests, StandaloneTemplateAndSyntaxValidation)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    // Initial standalone state should load the ECU template
    EXPECT_TRUE(panel.currentNode().isEmpty());
    EXPECT_FALSE(panel.isModified());

    // Valid template should pass syntax check without error
    panel.onCheckSyntax();

    // Now load a new script with the cyclic template
    panel.onNewScript(1);
    panel.onCheckSyntax();
}

TEST(ScriptEditorPanelTests, NodeBindingAndFileRoundtrip)
{
    GraphDescription description;
    NodeDescription ecuNode;
    ecuNode.id = "ecu_test";
    ecuNode.typeName = "lua.ecu";
    ecuNode.parameters.set(
        "script", ParameterValue::fromText("function on_start()\n    print('hello')\nend\n"));
    description.addNode(ecuNode);

    ScriptEditorPanel panel{description};
    panel.showNode(QStringLiteral("ecu_test"));

    EXPECT_EQ(panel.currentNode(), QStringLiteral("ecu_test"));
    EXPECT_FALSE(panel.isModified());

    // Save As to a temporary file
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("ecu_script.lua"));

    EXPECT_TRUE(panel.saveFile(filePath));
    EXPECT_TRUE(QFile::exists(filePath));

    // Open file into panel
    EXPECT_TRUE(panel.openFile(filePath));
    EXPECT_EQ(panel.currentNode(), QStringLiteral("ecu_test"));
}

TEST(ScriptEditTests, InsertSnippetWithIndentation)
{
    ScriptEdit editor;
    editor.setPlainText(QStringLiteral("function test()\n    -- cursor here\nend\n"));

    // Move cursor to line 2
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down);
    cursor.movePosition(QTextCursor::EndOfLine);
    editor.setTextCursor(cursor);

    // Insert a multi-line snippet
    editor.insertSnippet(QStringLiteral("\nlocal a = 1\nlocal b = 2"));

    const QString content = editor.toPlainText();
    EXPECT_TRUE(content.contains(QStringLiteral("    local a = 1")));
    EXPECT_TRUE(content.contains(QStringLiteral("    local b = 2")));
}

TEST(ScriptEditorPanelTests, SidebarToggleAndComponents)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    EXPECT_TRUE(panel.isSidebarVisible());

    panel.toggleSidebar(false);
    EXPECT_FALSE(panel.isSidebarVisible());

    panel.toggleSidebar(true);
    EXPECT_TRUE(panel.isSidebarVisible());

    // Check tree pointers are initialized
    EXPECT_NE(panel.signalsTree(), nullptr);
    EXPECT_NE(panel.functionsTree(), nullptr);
    EXPECT_NE(panel.variablesTree(), nullptr);

    // Functions tree should have pre-populated categories
    EXPECT_GT(panel.functionsTree()->topLevelItemCount(), 0);

    // Test functions filter
    panel.onFunctionFilterChanged(QStringLiteral("emit_signal"));
    // Filtering should keep matching function visible
    bool foundVisible = false;
    for (int i = 0; i < panel.functionsTree()->topLevelItemCount(); ++i) {
        auto* cat = panel.functionsTree()->topLevelItem(i);
        if (!cat->isHidden()) {
            foundVisible = true;
            break;
        }
    }
    EXPECT_TRUE(foundVisible);

    // Reset filter
    panel.onFunctionFilterChanged(QString{});
}

TEST(ScriptEditorPanelTests, DBCSignalsAndSystemVariablesPopulation)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    // 1. Create a dummy CAN database with one message and one signal
    auto db = std::make_shared<CanDatabase>();
    db->sourcePath = "D:/TorqueBus/databases/engine.dbc";

    CanMessage msg;
    msg.identifier = 0x100;
    msg.name = "EngineData";
    msg.length = 8;
    msg.cycleTimeMs = 100;

    CanSignal sig;
    sig.name = "EngineSpeed";
    sig.startBit = 0;
    sig.bitLength = 16;
    sig.unit = "rpm";
    sig.minimum = 0.0;
    sig.maximum = 8000.0;
    sig.comment = "Engine RPM sensor";
    msg.signalList.push_back(sig);

    db->addMessage(msg);

    // Hand to panel
    panel.setDatabases({db});

    EXPECT_EQ(panel.signalsTree()->topLevelItemCount(), 1);
    QTreeWidgetItem* dbItem = panel.signalsTree()->topLevelItem(0);
    EXPECT_TRUE(dbItem->text(0).contains(QStringLiteral("engine.dbc")));
    EXPECT_EQ(dbItem->childCount(), 1);

    QTreeWidgetItem* msgItem = dbItem->child(0);
    EXPECT_EQ(msgItem->text(0), QStringLiteral("EngineData"));
    EXPECT_EQ(msgItem->childCount(), 1);

    QTreeWidgetItem* sigItem = msgItem->child(0);
    EXPECT_EQ(sigItem->text(0), QStringLiteral("EngineSpeed"));

    // Double clicking signal inserts emit_signal snippet
    panel.onSignalItemDoubleClicked(sigItem, 0);

    // 2. Test SystemVariables
    SystemVariables vars;
    vars.set("EngineSpeed", 2500.0);
    vars.set("VehicleSpeed", 60.0);
    panel.setVariables(&vars);

    EXPECT_EQ(panel.variablesTree()->topLevelItemCount(), 2); // System Variables + Node Parameters
    QTreeWidgetItem* sysVarCat = panel.variablesTree()->topLevelItem(0);
    EXPECT_EQ(sysVarCat->childCount(), 2);
    EXPECT_EQ(sysVarCat->child(0)->text(0), QStringLiteral("EngineSpeed"));
    EXPECT_EQ(sysVarCat->child(0)->text(1), QStringLiteral("2500"));

    // Double clicking variable inserts var_get snippet
    panel.onVariableItemDoubleClicked(sysVarCat->child(0), 0);
}

TEST(ScriptEditorPanelTests, RevertRestoresCommittedScript)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    EXPECT_FALSE(panel.isModified());

    // Modify editor text
    panel.editor()->setPlainText(QStringLiteral("-- Modified user text"));
    EXPECT_TRUE(panel.isModified());

    // Call onRevert()
    panel.onRevert();
    EXPECT_FALSE(panel.isModified());
    EXPECT_TRUE(
        panel.editor()->toPlainText().contains(QStringLiteral("ECU simulation initialized")));
}

TEST(ScriptEditorPanelTests, FilterReapplicationAcrossTreeUpdates)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    // Apply a filter that matches emit_signal
    panel.onFunctionFilterChanged(QStringLiteral("emit_signal"));

    // Verify only the category containing emit_signal is visible
    int visibleCatCount = 0;
    for (int i = 0; i < panel.functionsTree()->topLevelItemCount(); ++i) {
        if (!panel.functionsTree()->topLevelItem(i)->isHidden()) {
            ++visibleCatCount;
        }
    }
    EXPECT_GT(visibleCatCount, 0);
    EXPECT_LT(visibleCatCount, panel.functionsTree()->topLevelItemCount());

    // Clear filter
    panel.onFunctionFilterChanged(QString{});
}

TEST(ScriptEditorPanelTests, TreeItemDoubleClickAndKeyActivationInsertsExactlyOnce)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};
    panel.editor()->clear();

    // Find function item in functions tree
    ASSERT_GT(panel.functionsTree()->topLevelItemCount(), 0);
    QTreeWidgetItem* cat = panel.functionsTree()->topLevelItem(0);
    ASSERT_GT(cat->childCount(), 0);
    QTreeWidgetItem* funcItem = cat->child(0);

    // Emulate double click via signal emission
    Q_EMIT panel.functionsTree()->itemDoubleClicked(funcItem, 0);

    const QString textAfterDoubleClick = panel.editor()->toPlainText();
    const QString funcName = funcItem->text(0);
    EXPECT_EQ(textAfterDoubleClick.count(funcName), 1);

    // Now test keyboard Enter eventFilter
    QKeyEvent enterKey{QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    panel.functionsTree()->setCurrentItem(funcItem);
    QApplication::sendEvent(panel.functionsTree(), &enterKey);

    const QString textAfterEnter = panel.editor()->toPlainText();
    EXPECT_EQ(textAfterEnter.count(funcName), 2);
}

} // namespace
} // namespace torquebus::ui
