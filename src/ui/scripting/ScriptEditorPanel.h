// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Script Editor: edit simulated ECUs and test sequences with a full-featured
// professional Lua IDE experience. Supports toolbar actions (New, Open, Save,
// Save As, Undo/Redo, Find/Replace, Commenting, Syntax Check, Hot Reload, Zoom),
// document status header, collapsible search bar, and live metrics.

#pragma once

#include "core/dashboard/SystemVariables.h"
#include "core/database/CanMessage.h"

#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QAction;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTextEdit;
class QToolBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus {
class GraphDescription;
class ScriptLibrary;
} // namespace torquebus

namespace torquebus::ui {

class LuaHighlighter;
class ScriptEdit;

class ScriptEditorPanel final : public QWidget {
    Q_OBJECT

public:
    /// The description is not owned and must outlive the panel.
    explicit ScriptEditorPanel(GraphDescription& description, QWidget* parent = nullptr);

    /// Where edited scripts are handed to running nodes. Not owned; null
    /// detaches, and the panel then only edits the project.
    void setLibrary(ScriptLibrary* library);

    /// Whether a measurement is running, deciding between "reload it now"
    /// and "it will be used at the next Start".
    void setRunning(bool running);

    /// Shows one node's script. A node that is not a `lua.ecu` or `lua.test`
    /// sets the editor into standalone mode.
    void showNode(const QString& descriptionId);

    /// The directory a relative `scriptPath` resolves against.
    void setBasePath(const QString& path) { m_basePath = path; }

    /// Updates the loaded databases for DBC symbol browsing and auto-completion.
    void setDatabases(const std::vector<std::shared_ptr<const CanDatabase>>& databases);

    /// Sets the SystemVariables instance for browsing dashboard & project variables.
    void setVariables(SystemVariables* variables);

    /// Shows or hides the Symbol & API Explorer sidebar.
    void toggleSidebar(bool visible);
    [[nodiscard]] bool isSidebarVisible() const;

    /// Direct access to symbol/API trees (for inspection / tests)
    [[nodiscard]] QTreeWidget* signalsTree() const noexcept { return m_signalsTree; }
    [[nodiscard]] QTreeWidget* functionsTree() const noexcept { return m_functionsTree; }
    [[nodiscard]] QTreeWidget* variablesTree() const noexcept { return m_variablesTree; }
    [[nodiscard]] ScriptEdit* editor() const noexcept { return m_editor; }

    void clear();

    [[nodiscard]] QString currentNode() const { return m_nodeId; }

    /// True while the editor holds text that has not been written back.
    [[nodiscard]] bool isModified() const;

    /// Direct file operations
    bool openFile(const QString& filePath);
    bool saveFile(const QString& filePath);

Q_SIGNALS:
    /// Emitted when a node's script is saved into the pipeline graph.
    void nodeEdited(const QString& descriptionId);

    /// A line for the Output panel - what a reload did, or why it was refused.
    void reported(const QString& text, bool isError);

public Q_SLOTS:
    void onNewScript(int templateType = 0);
    void onOpenFile();
    bool onSave();
    bool onSaveAs();

    void onCheckSyntax();
    void onReload();
    void onRevert();

    void toggleFindReplace(bool visible);
    void onFindNext();
    void onFindPrevious();
    void onReplace();
    void onReplaceAll();

    // Explorer sidebar slots
    void onSignalFilterChanged(const QString& filter);
    void onFunctionFilterChanged(const QString& filter);
    void onVariableFilterChanged(const QString& filter);
    void onSignalItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onFunctionItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onVariableItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onSignalItemSelectionChanged();
    void onFunctionItemSelectionChanged();
    void onVariableItemSelectionChanged();
    void onInsertSelectedSnippet();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    void refresh();
    void onTextChanged();
    void onCursorPositionChanged();
    void onThemeChanged();

private:
    void buildUi();
    void setupToolbar();
    void setupFindReplaceWidget();
    void setupSidebar();
    void setupSignalsTab(QWidget* parent);
    void setupFunctionsTab(QWidget* parent);
    void setupVariablesTab(QWidget* parent);
    void populateSignalsTree();
    void populateFunctionsTree();
    void populateVariablesTree();
    static bool applyTreeFilter(QTreeWidgetItem* item, const QString& needle);
    void setupStatusBar();
    void applyIcons();
    void updateAvailability();
    void updateStatusMetrics();
    void updateDocumentHeader();

    /// Writes the editor's text where this script lives - the file, or the
    /// node's parameter. Returns false and says why on a file it cannot write.
    bool commit(QString& error);

    /// Reads the script this node runs, from wherever it is kept.
    [[nodiscard]] QString readSource(bool& fromFile, QString& path) const;

    /// The line number in a Lua error - `ecu.lua:42: ...` - or 0.
    [[nodiscard]] static int errorLineOf(const QString& message);

    void setStatus(const QString& text, bool isError);

    GraphDescription& m_description;
    ScriptLibrary* m_library{nullptr};

    QString m_nodeId;
    QString m_basePath;

    /// True when this node's script is a file rather than inline text.
    bool m_isFile{false};
    QString m_filePath;

    bool m_running{false};

    /// What was last committed, so Revert has something to go back to.
    QString m_committed;

    // Toolbar
    QToolBar* m_toolBar{nullptr};
    QAction* m_actionNew{nullptr};
    QAction* m_actionOpen{nullptr};
    QAction* m_actionSave{nullptr};
    QAction* m_actionSaveAs{nullptr};
    QAction* m_actionRevert{nullptr};
    QAction* m_actionUndo{nullptr};
    QAction* m_actionRedo{nullptr};
    QAction* m_actionFind{nullptr};
    QAction* m_actionComment{nullptr};
    QAction* m_actionCheck{nullptr};
    QAction* m_actionReload{nullptr};
    QAction* m_actionToggleSidebar{nullptr};
    QAction* m_actionZoomIn{nullptr};
    QAction* m_actionZoomOut{nullptr};
    QAction* m_actionResetZoom{nullptr};

    // Central Splitter & Containers
    QSplitter* m_splitter{nullptr};
    QWidget* m_editorContainer{nullptr};
    QWidget* m_sidebarWidget{nullptr};
    int m_lastSidebarWidth{300};

    // Document Header Bar
    QWidget* m_headerBar{nullptr};
    QLabel* m_headerIcon{nullptr};
    QLabel* m_headerTitle{nullptr};
    QLabel* m_dirtyBadge{nullptr};

    // Find and Replace Widget
    QWidget* m_findWidget{nullptr};
    QLineEdit* m_findInput{nullptr};
    QLineEdit* m_replaceInput{nullptr};
    QToolButton* m_findNextBtn{nullptr};
    QToolButton* m_findPrevBtn{nullptr};
    QToolButton* m_matchCaseBtn{nullptr};
    QToolButton* m_wholeWordsBtn{nullptr};
    QPushButton* m_replaceBtn{nullptr};
    QPushButton* m_replaceAllBtn{nullptr};
    QLabel* m_matchCountLabel{nullptr};
    QToolButton* m_closeFindBtn{nullptr};

    // Code Editor & Highlighter
    ScriptEdit* m_editor{nullptr};
    LuaHighlighter* m_highlighter{nullptr};

    // Explorer Sidebar Components
    QTabWidget* m_sideTabs{nullptr};

    // Tab 1: Signals
    QLineEdit* m_signalFilterInput{nullptr};
    QTreeWidget* m_signalsTree{nullptr};

    // Tab 2: Functions
    QLineEdit* m_functionFilterInput{nullptr};
    QTreeWidget* m_functionsTree{nullptr};

    // Tab 3: Variables
    QLineEdit* m_variableFilterInput{nullptr};
    QTreeWidget* m_variablesTree{nullptr};

    // Sidebar bottom details & insert button
    QLabel* m_detailsHeader{nullptr};
    QTextEdit* m_detailsText{nullptr};
    QPushButton* m_insertSnippetBtn{nullptr};

    // Status Footer
    QWidget* m_footer{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_cursorMetrics{nullptr};
    QLabel* m_docMetrics{nullptr};
    QLabel* m_langBadge{nullptr};

    bool m_statusIsError{false};

    std::vector<std::shared_ptr<const CanDatabase>> m_databases;
    SystemVariables* m_variables{nullptr};
};

} // namespace torquebus::ui
