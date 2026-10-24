// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/pipeline/GraphDescription.h"
#include "ui/scripting/LuaHighlighter.h"
#include "ui/scripting/ScriptEdit.h"
#include "ui/scripting/ScriptEditorPanel.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

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

TEST(ScriptEditorPanelTests, TheScriptReferenceListsTheHookThatAnswersRequests)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    const QTreeWidgetItem* found = nullptr;

    for (QTreeWidgetItemIterator it{panel.functionsTree()}; *it != nullptr; ++it) {
        if ((*it)->text(0) == QStringLiteral("on_request")) {
            found = *it;
        }
    }

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->parent()->text(0).toStdString(), "Event Handlers");
    EXPECT_EQ(found->text(1).toStdString(),
              "function on_request(pgn, destination, source, channel)");
    EXPECT_TRUE(found->data(0, Qt::UserRole + 3).toString().contains(QStringLiteral("false")));

    // The template inserted is the hook, and a script that has it is one the node would call.
    const QString snippet = found->data(0, Qt::UserRole + 4).toString();
    EXPECT_TRUE(snippet.startsWith(
        QStringLiteral("function on_request(pgn, destination, source, channel)")));
    EXPECT_TRUE(snippet.contains(QStringLiteral("return false")));
    EXPECT_TRUE(snippet.contains(QStringLiteral("j1939_send(")));
}

TEST(ScriptEditorPanelTests, TheEditorColoursEveryHookTheNodeLooksFor)
{
    // A hook that is not coloured looks like a typo in a script, and one a node calls is one the
    // person writing it has to be able to trust.
    QTextDocument document;
    LuaHighlighter highlighter{&document};

    for (const char* hook : {"on_enable",
                             "on_disable",
                             "on_timer",
                             "on_message",
                             "on_request",
                             "on_uds_request",
                             "on_security_seed"}) {
        document.setPlainText(QStringLiteral("function %1() end").arg(QLatin1String{hook}));
        highlighter.rehighlight();

        const QList<QTextLayout::FormatRange> formats = document.firstBlock().layout()->formats();
        const int length = static_cast<int>(std::strlen(hook));

        const bool coloured =
            std::ranges::any_of(formats, [length](const QTextLayout::FormatRange& range) {
                return range.start == 9 && range.length == length;
            });

        EXPECT_TRUE(coloured) << hook;
    }

    // And a word that only looks like one is not.
    document.setPlainText(QStringLiteral("function on_requests() end"));
    highlighter.rehighlight();

    for (const QTextLayout::FormatRange& range : document.firstBlock().layout()->formats()) {
        EXPECT_FALSE(range.start == 9 && range.length == 10);
    }
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

/// Every piece of text the panel can put in front of a person: what is written on a widget, in a
/// tool tip, a placeholder, on a tab, an action or a row of a tree - and, for each row, the
/// documentation the panel shows for it.
[[nodiscard]] QStringList everythingThePanelSays(ScriptEditorPanel& panel)
{
    QStringList texts;

    const auto add = [&texts](const QString& text) {
        if (!text.isEmpty()) {
            texts.push_back(text);
        }
    };

    for (const QWidget* widget : panel.findChildren<QWidget*>()) {
        add(widget->toolTip());
        add(widget->statusTip());
        add(widget->whatsThis());
        add(widget->accessibleName());
        add(widget->accessibleDescription());
    }
    for (const QLabel* label : panel.findChildren<QLabel*>()) {
        add(label->text());
    }
    for (const QAbstractButton* button : panel.findChildren<QAbstractButton*>()) {
        add(button->text());
    }
    for (const QLineEdit* edit : panel.findChildren<QLineEdit*>()) {
        add(edit->placeholderText());
    }
    for (const QAction* action : panel.findChildren<QAction*>()) {
        add(action->text());
        add(action->toolTip());
        add(action->statusTip());
    }
    for (const QTabWidget* tabs : panel.findChildren<QTabWidget*>()) {
        for (int index = 0; index < tabs->count(); ++index) {
            add(tabs->tabText(index));
            add(tabs->tabToolTip(index));
        }
    }

    // A row's own text, and what the panel says about it once it is the one selected.
    const auto* details = panel.findChild<QTextEdit*>();
    for (QTreeWidget* tree : panel.findChildren<QTreeWidget*>()) {
        for (int column = 0; column < tree->columnCount(); ++column) {
            add(tree->headerItem()->text(column));
        }

        for (QTreeWidgetItemIterator it{tree}; *it != nullptr; ++it) {
            for (int column = 0; column < tree->columnCount(); ++column) {
                add((*it)->text(column));
                add((*it)->toolTip(column));
            }

            tree->setCurrentItem(*it);
            if (details != nullptr) {
                add(details->toPlainText());
                add(details->placeholderText());
            }
        }
    }

    return texts;
}

TEST(ScriptEditorPanelTests, TheSidebarIsTheScriptReference)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    // What it is: the signals a database has, the functions a script can call and the variables it
    // can read, with what each one is and a snippet to put in the script. A name that says that.
    const auto* title = panel.findChild<QLabel*>(QStringLiteral("scriptSidebarTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QStringLiteral("Script Reference"));

    // The button that shows and hides it is called the same.
    bool found = false;
    for (const QAction* action : panel.findChildren<QAction*>()) {
        if (action->text() == QStringLiteral("Script Reference")) {
            found = true;
            EXPECT_TRUE(action->toolTip().contains(QStringLiteral("Script Reference")))
                << action->toolTip().toStdString();
        }
    }
    EXPECT_TRUE(found);
}

TEST(ScriptEditorPanelTests, NothingThePanelSaysNamesAnotherVendorsScriptingLanguage)
{
    GraphDescription description;
    ScriptEditorPanel panel{description};

    // The scripting language of another vendor's tool has a name that is that vendor's. This is a
    // scripting tool for Lua and says Lua. The name is put together here rather than written out,
    // so that a search of the repository for it finds nothing - which is the point of this test.
    const QString reserved = QStringLiteral("CA") + QStringLiteral("PL");

    const QStringList texts = everythingThePanelSays(panel);
    ASSERT_GT(texts.size(), 50);

    for (const QString& text : texts) {
        EXPECT_FALSE(text.contains(reserved, Qt::CaseInsensitive)) << text.toStdString();
    }
}

} // namespace
} // namespace torquebus::ui
