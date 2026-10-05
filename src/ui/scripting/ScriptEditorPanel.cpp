// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/scripting/ScriptEditorPanel.h"

#include "core/pipeline/GraphDescription.h"
#include "core/scripting/LuaRuntime.h"
#include "core/scripting/ScriptLibrary.h"
#include "ui/scripting/LuaHighlighter.h"
#include "ui/scripting/ScriptEdit.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <string>
#include <vector>

namespace torquebus::ui {
namespace {

constexpr int kRefreshMs = 50;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

const char* kEcuTemplate =
    "-- TorqueBus Lua ECU Simulation Script\n"
    "-- Runs in its own isolated Lua 5.5 runtime with microsecond precision.\n\n"
    "local cycle_count = 0\n"
    "local speed_kmh = 0.0\n\n"
    "-- Called once when measurement starts or when the script is hot-reloaded\n"
    "function on_start()\n"
    "    log_message(1, \"ECU simulation initialized on channel \" .. tostring(channel))\n"
    "    -- Configure periodic execution: 50 ms (20 Hz)\n"
    "    set_timer(50)\n"
    "end\n\n"
    "-- Called periodically if configured via set_timer(interval_ms)\n"
    "function on_timer(interval_ms)\n"
    "    cycle_count = cycle_count + 1\n"
    "    speed_kmh = 55.0 + 12.0 * math.sin(cycle_count * 0.05)\n\n"
    "    -- Example: Emit CAN frame (ID 0x100, 8 bytes: counter, speed, reserved)\n"
    "    -- local payload = string.pack(\">I2I2I4\", cycle_count % 65536, math.floor(speed_kmh * "
    "10), 0)\n"
    "    -- emit(0x100, payload)\n"
    "end\n\n"
    "-- Called whenever an incoming CAN frame arrives at this ECU\n"
    "function on_message(id, payload)\n"
    "    -- if id == 0x200 then\n"
    "    --     log_message(1, string.format(\"Received frame 0x%X, length: %d\", id, #payload))\n"
    "    -- end\n"
    "end\n\n"
    "-- Called when measurement stops\n"
    "function on_stop()\n"
    "    log_message(1, \"ECU simulation stopped\")\n"
    "end\n";

const char* kCyclicTemplate = "-- TorqueBus Cyclic CAN Transmitter Script\n"
                              "local counter = 0\n\n"
                              "function on_start()\n"
                              "    -- Set periodic timer for 20 ms (50 Hz)\n"
                              "    set_timer(20)\n"
                              "end\n\n"
                              "function on_timer(interval_ms)\n"
                              "    counter = (counter + 1) % 256\n"
                              "    local payload = string.pack(\"B B B B B B B B\", counter, 0x11, "
                              "0x22, 0x33, 0x44, 0x55, 0x66, 0x77)\n"
                              "    emit(0x280, payload)\n"
                              "end\n";

const char* kBlankTemplate = "-- TorqueBus Lua Script\n\n"
                             "function on_start()\n"
                             "    -- initialization\n"
                             "end\n\n"
                             "function on_message(id, payload)\n"
                             "    -- frame handler\n"
                             "end\n";

} // namespace

ScriptEditorPanel::ScriptEditorPanel(GraphDescription& description, QWidget* parent)
    : QWidget{parent}
    , m_description{description}
{
    buildUi();

    auto* timer = new QTimer(this);
    timer->setInterval(kRefreshMs);
    connect(timer, &QTimer::timeout, this, &ScriptEditorPanel::refresh);
    timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }

    clear();
}

void ScriptEditorPanel::buildUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 1. Professional Top Toolbar
    setupToolbar();
    rootLayout->addWidget(m_toolBar);

    // 2. Central Splitter (Editor on left, Explorer Sidebar on right)
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);

    // Left Container: Editor
    m_editorContainer = new QWidget(m_splitter);
    auto* editorLayout = new QVBoxLayout(m_editorContainer);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);

    // Document Information & Header Bar
    m_headerBar = new QWidget(m_editorContainer);
    auto* headerLayout = new QHBoxLayout(m_headerBar);
    headerLayout->setContentsMargins(8, 4, 8, 4);
    headerLayout->setSpacing(8);

    m_headerIcon = new QLabel(m_headerBar);
    headerLayout->addWidget(m_headerIcon);

    m_headerTitle = new QLabel(m_headerBar);
    m_headerTitle->setProperty("torquebusRole", QStringLiteral("caption"));
    m_headerTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerLayout->addWidget(m_headerTitle, 1);

    m_dirtyBadge = new QLabel(m_headerBar);
    m_dirtyBadge->setProperty("torquebusRole", QStringLiteral("caption"));
    headerLayout->addWidget(m_dirtyBadge);

    editorLayout->addWidget(m_headerBar);

    // Collapsible Find & Replace Bar
    setupFindReplaceWidget();
    editorLayout->addWidget(m_findWidget);

    // Code Editor & Lua Syntax Highlighter
    m_editor = new ScriptEdit(m_editorContainer);
    m_highlighter = new LuaHighlighter(m_editor->document());

    connect(m_editor, &ScriptEdit::textChanged, this, &ScriptEditorPanel::onTextChanged);
    connect(m_editor,
            &ScriptEdit::cursorPositionChanged,
            this,
            &ScriptEditorPanel::onCursorPositionChanged);

    editorLayout->addWidget(m_editor, 1);
    m_splitter->addWidget(m_editorContainer);

    // Right Container: Explorer Sidebar
    setupSidebar();
    m_splitter->addWidget(m_sidebarWidget);

    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setSizes({720, 320});

    rootLayout->addWidget(m_splitter, 1);

    // 3. Status Footer & Live Metrics
    setupStatusBar();
    rootLayout->addWidget(m_footer);

    applyIcons();
}

void ScriptEditorPanel::setupToolbar()
{
    m_toolBar = new QToolBar(this);
    m_toolBar->setObjectName(QStringLiteral("torquebus.toolbar.script"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setIconSize(QSize{16, 16});
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    ThemeManager* themes = ThemeManager::instance();
    const auto getIcon = [themes](const QString& name) {
        return themes != nullptr ? themes->icon(name) : QIcon{};
    };

    // --- New Script with Template Menu ------------------------------------
    m_actionNew = m_toolBar->addAction(getIcon(QStringLiteral("new")), tr("New Script"));
    m_actionNew->setToolTip(tr("Create a new script (Ctrl+N)"));
    m_actionNew->setShortcut(QKeySequence::New);

    auto* newMenu = new QMenu(this);
    auto* actEcu = newMenu->addAction(tr("Simulated ECU Template"));
    connect(actEcu, &QAction::triggered, this, [this] { onNewScript(0); });
    auto* actCyclic = newMenu->addAction(tr("Cyclic Frame Sender Template"));
    connect(actCyclic, &QAction::triggered, this, [this] { onNewScript(1); });
    auto* actBlank = newMenu->addAction(tr("Blank Script"));
    connect(actBlank, &QAction::triggered, this, [this] { onNewScript(2); });

    m_actionNew->setMenu(newMenu);
    connect(m_actionNew, &QAction::triggered, this, [this] { onNewScript(0); });

    if (auto* newBtn = qobject_cast<QToolButton*>(m_toolBar->widgetForAction(m_actionNew))) {
        newBtn->setObjectName(QStringLiteral("newScriptButton"));
        newBtn->setPopupMode(QToolButton::MenuButtonPopup);
    }

    // --- Open / Save / Save As --------------------------------------------
    m_actionOpen = m_toolBar->addAction(getIcon(QStringLiteral("open")), tr("Open Script..."));
    m_actionOpen->setToolTip(tr("Open an existing Lua script file (Ctrl+O)"));
    m_actionOpen->setShortcut(QKeySequence::Open);
    connect(m_actionOpen, &QAction::triggered, this, &ScriptEditorPanel::onOpenFile);

    m_actionSave = m_toolBar->addAction(getIcon(QStringLiteral("save")), tr("Save"));
    m_actionSave->setToolTip(tr("Save script changes (Ctrl+S)"));
    m_actionSave->setShortcut(QKeySequence::Save);
    connect(m_actionSave, &QAction::triggered, this, &ScriptEditorPanel::onSave);

    m_actionSaveAs = m_toolBar->addAction(getIcon(QStringLiteral("save-as")), tr("Save As..."));
    m_actionSaveAs->setToolTip(tr("Save script to a new file (Ctrl+Shift+S)"));
    m_actionSaveAs->setShortcut(QKeySequence::SaveAs);
    connect(m_actionSaveAs, &QAction::triggered, this, &ScriptEditorPanel::onSaveAs);

    m_actionRevert = m_toolBar->addAction(getIcon(QStringLiteral("clear")), tr("Revert"));
    m_actionRevert->setToolTip(tr("Discard unsaved changes and revert to last saved state"));
    connect(m_actionRevert, &QAction::triggered, this, &ScriptEditorPanel::onRevert);

    m_toolBar->addSeparator();

    // --- Undo / Redo ------------------------------------------------------
    m_actionUndo = m_toolBar->addAction(getIcon(QStringLiteral("undo")), tr("Undo"));
    m_actionUndo->setToolTip(tr("Undo last change (Ctrl+Z)"));
    m_actionUndo->setShortcut(QKeySequence::Undo);
    connect(m_actionUndo, &QAction::triggered, m_editor, &ScriptEdit::undo);

    m_actionRedo = m_toolBar->addAction(getIcon(QStringLiteral("redo")), tr("Redo"));
    m_actionRedo->setToolTip(tr("Redo last change (Ctrl+Y)"));
    m_actionRedo->setShortcut(QKeySequence::Redo);
    connect(m_actionRedo, &QAction::triggered, m_editor, &ScriptEdit::redo);

    m_toolBar->addSeparator();

    // --- Find & Replace / Comment -----------------------------------------
    m_actionFind = m_toolBar->addAction(getIcon(QStringLiteral("search")), tr("Find & Replace"));
    m_actionFind->setToolTip(tr("Find & Replace in script (Ctrl+F)"));
    m_actionFind->setShortcut(QKeySequence::Find);
    m_actionFind->setCheckable(true);
    connect(m_actionFind, &QAction::toggled, this, &ScriptEditorPanel::toggleFindReplace);

    m_actionComment =
        m_toolBar->addAction(getIcon(QStringLiteral("comment")), tr("Toggle Comment"));
    m_actionComment->setToolTip(tr("Comment or uncomment selected lines (Ctrl+/)"));
    m_actionComment->setShortcut(QKeySequence{Qt::CTRL | Qt::Key_Slash});
    connect(m_actionComment, &QAction::triggered, m_editor, &ScriptEdit::toggleComment);

    m_toolBar->addSeparator();

    // --- Check Syntax / Reload --------------------------------------------
    m_actionCheck = m_toolBar->addAction(getIcon(QStringLiteral("check")), tr("Check Syntax"));
    m_actionCheck->setToolTip(tr("Check Lua syntax without executing (F7)"));
    m_actionCheck->setShortcut(QKeySequence{Qt::Key_F7});
    connect(m_actionCheck, &QAction::triggered, this, &ScriptEditorPanel::onCheckSyntax);

    m_actionReload = m_toolBar->addAction(getIcon(QStringLiteral("replay")), tr("Reload"));
    m_actionReload->setToolTip(tr("Save and hot-reload script in running ECU (F5)"));
    m_actionReload->setShortcut(QKeySequence{Qt::Key_F5});
    connect(m_actionReload, &QAction::triggered, this, &ScriptEditorPanel::onReload);

    m_toolBar->addSeparator();

    // --- Zoom In / Out / Reset -------------------------------------------
    m_actionZoomIn = m_toolBar->addAction(getIcon(QStringLiteral("zoom-in")), tr("Zoom In"));
    m_actionZoomIn->setToolTip(tr("Increase font size (Ctrl++)"));
    m_actionZoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(m_actionZoomIn, &QAction::triggered, this, [this] { m_editor->zoomIn(1.0); });

    m_actionZoomOut = m_toolBar->addAction(getIcon(QStringLiteral("zoom-out")), tr("Zoom Out"));
    m_actionZoomOut->setToolTip(tr("Decrease font size (Ctrl+-)"));
    m_actionZoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(m_actionZoomOut, &QAction::triggered, this, [this] { m_editor->zoomOut(1.0); });

    m_actionResetZoom = m_toolBar->addAction(tr("100%"));
    m_actionResetZoom->setToolTip(tr("Reset font zoom to default 10pt (Ctrl+0)"));
    m_actionResetZoom->setShortcut(QKeySequence{Qt::CTRL | Qt::Key_0});
    connect(m_actionResetZoom, &QAction::triggered, this, [this] { m_editor->resetZoom(); });

    m_toolBar->addSeparator();

    // --- Toggle Symbol & API Explorer Sidebar -----------------------------
    m_actionToggleSidebar =
        m_toolBar->addAction(getIcon(QStringLiteral("properties")), tr("Symbol Explorer"));
    m_actionToggleSidebar->setToolTip(tr("Show or hide the Symbol & API Explorer sidebar (Ctrl+B)"));
    m_actionToggleSidebar->setShortcut(QKeySequence{Qt::CTRL | Qt::Key_B});
    m_actionToggleSidebar->setCheckable(true);
    m_actionToggleSidebar->setChecked(true);
    connect(m_actionToggleSidebar, &QAction::toggled, this, &ScriptEditorPanel::toggleSidebar);
}

void ScriptEditorPanel::setupFindReplaceWidget()
{
    m_findWidget = new QWidget(this);
    m_findWidget->setObjectName(QStringLiteral("findReplaceBar"));
    m_findWidget->setVisible(false);

    auto* layout = new QVBoxLayout(m_findWidget);
    layout->setContentsMargins(8, 4, 8, 6);
    layout->setSpacing(4);

    ThemeManager* themes = ThemeManager::instance();
    const auto getIcon = [themes](const QString& name) {
        return themes != nullptr ? themes->icon(name) : QIcon{};
    };

    // Row 1: Find input and search navigation
    auto* row1 = new QHBoxLayout;
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(4);

    auto* findIcon = new QLabel(m_findWidget);
    findIcon->setPixmap(getIcon(QStringLiteral("search")).pixmap(14, 14));
    row1->addWidget(findIcon);

    m_findInput = new QLineEdit(m_findWidget);
    m_findInput->setPlaceholderText(tr("Find in script..."));
    m_findInput->setClearButtonEnabled(true);
    connect(m_findInput, &QLineEdit::returnPressed, this, &ScriptEditorPanel::onFindNext);
    connect(m_findInput, &QLineEdit::textChanged, this, &ScriptEditorPanel::onFindNext);
    row1->addWidget(m_findInput, 1);

    m_findNextBtn = new QToolButton(m_findWidget);
    m_findNextBtn->setText(tr("Next"));
    m_findNextBtn->setToolTip(tr("Find next occurrence (F3 / Enter)"));
    connect(m_findNextBtn, &QToolButton::clicked, this, &ScriptEditorPanel::onFindNext);
    row1->addWidget(m_findNextBtn);

    m_findPrevBtn = new QToolButton(m_findWidget);
    m_findPrevBtn->setText(tr("Prev"));
    m_findPrevBtn->setToolTip(tr("Find previous occurrence (Shift+F3)"));
    connect(m_findPrevBtn, &QToolButton::clicked, this, &ScriptEditorPanel::onFindPrevious);
    row1->addWidget(m_findPrevBtn);

    m_matchCaseBtn = new QToolButton(m_findWidget);
    m_matchCaseBtn->setText(tr("Aa"));
    m_matchCaseBtn->setCheckable(true);
    m_matchCaseBtn->setToolTip(tr("Match Case"));
    connect(m_matchCaseBtn, &QToolButton::toggled, this, &ScriptEditorPanel::onFindNext);
    row1->addWidget(m_matchCaseBtn);

    m_wholeWordsBtn = new QToolButton(m_findWidget);
    m_wholeWordsBtn->setText(tr("\\b"));
    m_wholeWordsBtn->setCheckable(true);
    m_wholeWordsBtn->setToolTip(tr("Match Whole Words"));
    connect(m_wholeWordsBtn, &QToolButton::toggled, this, &ScriptEditorPanel::onFindNext);
    row1->addWidget(m_wholeWordsBtn);

    m_matchCountLabel = new QLabel(m_findWidget);
    m_matchCountLabel->setProperty("torquebusRole", QStringLiteral("caption"));
    row1->addWidget(m_matchCountLabel);

    m_closeFindBtn = new QToolButton(m_findWidget);
    m_closeFindBtn->setIcon(getIcon(QStringLiteral("close")));
    m_closeFindBtn->setIconSize(QSize{12, 12});
    m_closeFindBtn->setToolTip(tr("Close Find Bar (Esc)"));
    connect(m_closeFindBtn, &QToolButton::clicked, this, [this] { toggleFindReplace(false); });
    row1->addWidget(m_closeFindBtn);

    layout->addLayout(row1);

    // Row 2: Replace input and replace buttons
    auto* row2 = new QHBoxLayout;
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(4);

    auto* replaceIcon = new QLabel(m_findWidget);
    replaceIcon->setPixmap(getIcon(QStringLiteral("edit")).pixmap(14, 14));
    row2->addWidget(replaceIcon);

    m_replaceInput = new QLineEdit(m_findWidget);
    m_replaceInput->setPlaceholderText(tr("Replace with..."));
    m_replaceInput->setClearButtonEnabled(true);
    connect(m_replaceInput, &QLineEdit::returnPressed, this, &ScriptEditorPanel::onReplace);
    row2->addWidget(m_replaceInput, 1);

    m_replaceBtn = new QPushButton(tr("Replace"), m_findWidget);
    m_replaceBtn->setToolTip(tr("Replace current match"));
    connect(m_replaceBtn, &QPushButton::clicked, this, &ScriptEditorPanel::onReplace);
    row2->addWidget(m_replaceBtn);

    m_replaceAllBtn = new QPushButton(tr("Replace All"), m_findWidget);
    m_replaceAllBtn->setToolTip(tr("Replace all matches in document"));
    connect(m_replaceAllBtn, &QPushButton::clicked, this, &ScriptEditorPanel::onReplaceAll);
    row2->addWidget(m_replaceAllBtn);

    layout->addLayout(row2);

    auto* escShortcut = new QShortcut(QKeySequence{Qt::Key_Escape}, m_findWidget);
    connect(escShortcut, &QShortcut::activated, this, [this] { toggleFindReplace(false); });
}

void ScriptEditorPanel::setupStatusBar()
{
    m_footer = new QWidget(this);
    auto* layout = new QHBoxLayout(m_footer);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(12);

    m_status = new QLabel(m_footer);
    m_status->setProperty("torquebusRole", QStringLiteral("caption"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status, 1);

    m_cursorMetrics = new QLabel(m_footer);
    m_cursorMetrics->setProperty("torquebusRole", QStringLiteral("caption"));
    layout->addWidget(m_cursorMetrics);

    m_docMetrics = new QLabel(m_footer);
    m_docMetrics->setProperty("torquebusRole", QStringLiteral("caption"));
    layout->addWidget(m_docMetrics);

    m_langBadge = new QLabel(m_footer);
    m_langBadge->setProperty("torquebusRole", QStringLiteral("caption"));
    m_langBadge->setText(tr("Lua 5.5  ·  UTF-8"));
    layout->addWidget(m_langBadge);
}

void ScriptEditorPanel::applyIcons()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr) {
        return;
    }

    if (m_actionNew != nullptr) {
        m_actionNew->setIcon(themes->icon(QStringLiteral("new")));
    }
    if (m_actionOpen != nullptr) {
        m_actionOpen->setIcon(themes->icon(QStringLiteral("open")));
    }
    if (m_actionSave != nullptr) {
        m_actionSave->setIcon(themes->icon(QStringLiteral("save")));
    }
    if (m_actionSaveAs != nullptr) {
        m_actionSaveAs->setIcon(themes->icon(QStringLiteral("save-as")));
    }
    if (m_actionRevert != nullptr) {
        m_actionRevert->setIcon(themes->icon(QStringLiteral("clear")));
    }
    if (m_actionUndo != nullptr) {
        m_actionUndo->setIcon(themes->icon(QStringLiteral("undo")));
    }
    if (m_actionRedo != nullptr) {
        m_actionRedo->setIcon(themes->icon(QStringLiteral("redo")));
    }
    if (m_actionFind != nullptr) {
        m_actionFind->setIcon(themes->icon(QStringLiteral("search")));
    }
    if (m_actionComment != nullptr) {
        m_actionComment->setIcon(themes->icon(QStringLiteral("comment")));
    }
    if (m_actionCheck != nullptr) {
        m_actionCheck->setIcon(themes->icon(QStringLiteral("check")));
    }
    if (m_actionReload != nullptr) {
        m_actionReload->setIcon(themes->icon(QStringLiteral("replay")));
    }
    if (m_actionZoomIn != nullptr) {
        m_actionZoomIn->setIcon(themes->icon(QStringLiteral("zoom-in")));
    }
    if (m_actionZoomOut != nullptr) {
        m_actionZoomOut->setIcon(themes->icon(QStringLiteral("zoom-out")));
    }
    if (m_closeFindBtn != nullptr) {
        m_closeFindBtn->setIcon(themes->icon(QStringLiteral("close")));
    }
    if (m_actionToggleSidebar != nullptr) {
        m_actionToggleSidebar->setIcon(themes->icon(QStringLiteral("properties")));
    }
    if (m_sideTabs != nullptr) {
        m_sideTabs->setTabIcon(0, themes->icon(QStringLiteral("signal")));
        m_sideTabs->setTabIcon(1, themes->icon(QStringLiteral("script")));
        m_sideTabs->setTabIcon(2, themes->icon(QStringLiteral("gauge")));
    }
    if (m_insertSnippetBtn != nullptr) {
        m_insertSnippetBtn->setIcon(themes->icon(QStringLiteral("add")));
    }
    if (m_headerIcon != nullptr) {
        m_headerIcon->setPixmap(themes->icon(QStringLiteral("script")).pixmap(14, 14));
    }
}

void ScriptEditorPanel::setLibrary(ScriptLibrary* library)
{
    m_library = library;
}

void ScriptEditorPanel::setRunning(bool running)
{
    if (m_running == running) {
        return;
    }

    m_running = running;
    updateAvailability();
}

bool ScriptEditorPanel::isModified() const
{
    return m_editor != nullptr && m_editor->toPlainText() != m_committed;
}

void ScriptEditorPanel::clear()
{
    m_nodeId.clear();
    m_isFile = false;
    m_filePath.clear();

    if (m_editor != nullptr) {
        const QSignalBlocker blocker{m_editor};
        m_editor->setPlainText(QString::fromUtf8(kEcuTemplate));
        m_editor->setErrorLine(0);
    }
    m_committed = m_editor != nullptr ? m_editor->toPlainText() : QString{};

    updateDocumentHeader();
    updateStatusMetrics();
    updateAvailability();
    setStatus(tr("Ready. Editing standalone script."), false);
    populateVariablesTree();
}

void ScriptEditorPanel::showNode(const QString& descriptionId)
{
    const NodeDescription* node =
        descriptionId.isEmpty() ? nullptr : m_description.find(descriptionId.toStdString());

    // Lua ECUs and Test Sequences have a script.
    if (node == nullptr || (node->typeName != "lua.ecu" && node->typeName != "lua.test")) {
        if (m_nodeId.isEmpty()) {
            clear();
        }
        return;
    }

    if (descriptionId == m_nodeId) {
        return;
    }

    if (isModified() && isVisible()) {
        const auto reply =
            QMessageBox::question(this,
                                  tr("Unsaved Changes"),
                                  tr("The script for \"%1\" has unsaved changes. Do you want to save "
                                     "before switching?")
                                      .arg(m_nodeId.isEmpty() ? tr("Untitled") : m_nodeId),
                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

        if (reply == QMessageBox::Save) {
            if (!onSave()) {
                return;
            }
        } else if (reply == QMessageBox::Cancel) {
            return;
        }
    }

    m_nodeId = descriptionId;

    QString path;
    bool fromFile = false;
    const QString source = readSource(fromFile, path);

    m_isFile = fromFile;
    m_filePath = path;
    m_committed = source;

    {
        const QSignalBlocker blocker{m_editor};
        m_editor->setPlainText(source);
        m_editor->setErrorLine(0);
    }

    updateDocumentHeader();
    updateStatusMetrics();
    updateAvailability();
    setStatus(QString{}, false);
    populateVariablesTree();
}

void ScriptEditorPanel::updateDocumentHeader()
{
    if (m_headerTitle == nullptr) {
        return;
    }

    const Theme theme = currentTheme();

    if (!m_nodeId.isEmpty()) {
        if (m_isFile) {
            m_headerTitle->setText(
                tr("ECU Block: <b>%1</b>  ·  %2").arg(m_nodeId, QFileInfo{m_filePath}.fileName()));
            m_headerTitle->setToolTip(m_filePath);
        } else {
            m_headerTitle->setText(
                tr("ECU Block: <b>%1</b>  ·  inline project script").arg(m_nodeId));
            m_headerTitle->setToolTip(tr("Inline script embedded directly in project graph"));
        }
    } else {
        if (m_isFile && !m_filePath.isEmpty()) {
            m_headerTitle->setText(
                tr("Standalone Script  ·  %1").arg(QFileInfo{m_filePath}.fileName()));
            m_headerTitle->setToolTip(m_filePath);
        } else {
            m_headerTitle->setText(tr("Standalone Script (Unlinked)  ·  untitled.lua"));
            m_headerTitle->setToolTip(tr("Standalone script not yet linked to an ECU block"));
        }
    }

    if (m_dirtyBadge != nullptr) {
        const bool dirty = isModified();
        m_dirtyBadge->setText(dirty ? tr("● Unsaved") : tr("✓ Saved"));
        m_dirtyBadge->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;")
                                        .arg(dirty ? theme.accent.name() : theme.success.name()));
    }
}

QString ScriptEditorPanel::readSource(bool& fromFile, QString& path) const
{
    fromFile = false;
    path.clear();

    const NodeDescription* node = m_description.find(m_nodeId.toStdString());
    if (node == nullptr) {
        return {};
    }

    if (node->parameters.contains("scriptPath")) {
        fromFile = true;

        const QString declared = QString::fromStdString(node->parameters.text("scriptPath"));
        path = QDir::isAbsolutePath(declared) || m_basePath.isEmpty()
                   ? declared
                   : QDir{m_basePath}.filePath(declared);

        QFile file{path};
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }

        QTextStream stream{&file};
        return stream.readAll();
    }

    return QString::fromStdString(node->parameters.text("script"));
}

bool ScriptEditorPanel::commit(QString& error)
{
    const QString source = m_editor->toPlainText();

    if (m_isFile && !m_filePath.isEmpty()) {
        QFile file{m_filePath};

        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            error = tr("Cannot write %1: %2").arg(m_filePath, file.errorString());
            return false;
        }

        QTextStream stream{&file};
        stream << source;

        if (!file.flush()) {
            error = tr("Cannot write %1: %2").arg(m_filePath, file.errorString());
            return false;
        }

        file.close();
    } else if (!m_nodeId.isEmpty()) {
        NodeDescription* node =
            const_cast<NodeDescription*>(m_description.find(m_nodeId.toStdString()));

        if (node == nullptr) {
            error = tr("The block %1 is no longer on the canvas.").arg(m_nodeId);
            return false;
        }

        node->parameters.set("script", ParameterValue::fromText(source.toStdString()));
        Q_EMIT nodeEdited(m_nodeId);
    } else {
        // Standalone script with no file yet
        return onSaveAs();
    }

    m_committed = source;
    updateDocumentHeader();
    updateAvailability();

    return true;
}

void ScriptEditorPanel::onNewScript(int templateType)
{
    if (isModified()) {
        const auto reply =
            QMessageBox::question(this,
                                  tr("Unsaved Changes"),
                                  tr("The current script has unsaved changes. Do you want to save "
                                     "before creating a new script?"),
                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

        if (reply == QMessageBox::Save) {
            if (!onSave()) {
                return;
            }
        } else if (reply == QMessageBox::Cancel) {
            return;
        }
    }

    const char* tpl = kEcuTemplate;
    if (templateType == 1) {
        tpl = kCyclicTemplate;
    } else if (templateType == 2) {
        tpl = kBlankTemplate;
    }

    m_nodeId.clear();
    m_isFile = false;
    m_filePath.clear();

    m_editor->setPlainText(QString::fromUtf8(tpl));
    m_editor->setErrorLine(0);
    m_committed = m_editor->toPlainText();

    updateDocumentHeader();
    updateStatusMetrics();
    updateAvailability();
    setStatus(tr("New script created from template."), false);
}

void ScriptEditorPanel::onOpenFile()
{
    if (isModified()) {
        const auto reply =
            QMessageBox::question(this,
                                  tr("Unsaved Changes"),
                                  tr("The current script has unsaved changes. Do you want to save "
                                     "before opening a file?"),
                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

        if (reply == QMessageBox::Save) {
            if (!onSave()) {
                return;
            }
        } else if (reply == QMessageBox::Cancel) {
            return;
        }
    }

    const QString startDir = !m_filePath.isEmpty()
                                 ? QFileInfo{m_filePath}.absolutePath()
                                 : (!m_basePath.isEmpty() ? m_basePath : QDir::currentPath());

    const QString fileName = QFileDialog::getOpenFileName(
        this,
        tr("Open Lua Script"),
        startDir,
        tr("Lua Scripts (*.lua);;Text Files (*.txt);;All Files (*.*)"));

    if (fileName.isEmpty()) {
        return;
    }

    openFile(fileName);
}

bool ScriptEditorPanel::openFile(const QString& filePath)
{
    QFile file{filePath};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStatus(tr("Cannot open %1: %2").arg(filePath, file.errorString()), true);
        return false;
    }

    QTextStream stream{&file};
    const QString content = stream.readAll();
    file.close();

    m_filePath = filePath;
    m_isFile = true;
    m_committed = content;

    {
        const QSignalBlocker blocker{m_editor};
        m_editor->setPlainText(content);
        m_editor->setErrorLine(0);
    }

    // If an ECU node is selected, link the file to it
    if (!m_nodeId.isEmpty()) {
        NodeDescription* node =
            const_cast<NodeDescription*>(m_description.find(m_nodeId.toStdString()));
        if (node != nullptr) {
            QString relativePath = m_filePath;
            if (!m_basePath.isEmpty() && m_filePath.startsWith(m_basePath)) {
                relativePath = QDir{m_basePath}.relativeFilePath(m_filePath);
            }
            node->parameters.set("scriptPath",
                                 ParameterValue::fromText(relativePath.toStdString()));
            node->parameters.erase("script");
            Q_EMIT nodeEdited(m_nodeId);
        }
    }

    updateDocumentHeader();
    updateStatusMetrics();
    updateAvailability();
    setStatus(tr("✓ Opened %1").arg(QFileInfo{m_filePath}.fileName()), false);
    return true;
}

bool ScriptEditorPanel::onSave()
{
    if (m_isFile && !m_filePath.isEmpty()) {
        return saveFile(m_filePath);
    }
    if (!m_nodeId.isEmpty()) {
        QString error;
        if (!commit(error)) {
            setStatus(error, true);
            return false;
        }
        setStatus(tr("✓ Script saved to block %1").arg(m_nodeId), false);
        return true;
    }
    return onSaveAs();
}

bool ScriptEditorPanel::onSaveAs()
{
    QString defaultName = !m_filePath.isEmpty()
                              ? m_filePath
                              : (!m_nodeId.isEmpty() ? (m_nodeId + QStringLiteral(".lua"))
                                                     : QStringLiteral("script.lua"));

    const QString startDir =
        !m_basePath.isEmpty() ? QDir{m_basePath}.filePath(defaultName) : defaultName;

    const QString fileName = QFileDialog::getSaveFileName(
        this, tr("Save Lua Script As"), startDir, tr("Lua Scripts (*.lua);;All Files (*.*)"));

    if (fileName.isEmpty()) {
        return false;
    }

    return saveFile(fileName);
}

bool ScriptEditorPanel::saveFile(const QString& filePath)
{
    QFile file{filePath};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        const QString err = tr("Cannot write %1: %2").arg(filePath, file.errorString());
        setStatus(err, true);
        return false;
    }

    QTextStream stream{&file};
    stream << m_editor->toPlainText();
    if (!file.flush()) {
        const QString err = tr("Cannot flush %1: %2").arg(filePath, file.errorString());
        setStatus(err, true);
        return false;
    }
    file.close();

    m_filePath = filePath;
    m_isFile = true;
    m_committed = m_editor->toPlainText();

    if (!m_nodeId.isEmpty()) {
        NodeDescription* node =
            const_cast<NodeDescription*>(m_description.find(m_nodeId.toStdString()));
        if (node != nullptr) {
            QString relativePath = m_filePath;
            if (!m_basePath.isEmpty() && m_filePath.startsWith(m_basePath)) {
                relativePath = QDir{m_basePath}.relativeFilePath(m_filePath);
            }
            node->parameters.set("scriptPath",
                                 ParameterValue::fromText(relativePath.toStdString()));
            node->parameters.erase("script");
            Q_EMIT nodeEdited(m_nodeId);
        }
    }

    updateDocumentHeader();
    updateAvailability();
    setStatus(tr("✓ Successfully saved to %1").arg(QFileInfo{m_filePath}.fileName()), false);
    return true;
}

void ScriptEditorPanel::onCheckSyntax()
{
    if (m_editor == nullptr) {
        return;
    }

    const QString source = m_editor->toPlainText();
    const std::string chunk =
        m_isFile ? QFileInfo{m_filePath}.fileName().toStdString()
                 : (m_nodeId.isEmpty() ? "script.lua" : (m_nodeId.toStdString() + ".lua"));

    LuaRuntime runtime;
    (void)runtime.openLibraries();
    const Result result = runtime.checkSyntax(source.toStdString(), chunk);

    if (result.succeeded()) {
        m_editor->setErrorLine(0);
        setStatus(tr("✓ Syntax check passed: No errors found."), false);
    } else {
        const QString message = QString::fromStdString(std::string{result.message()});
        const int line = errorLineOf(message);
        m_editor->setErrorLine(line);
        setStatus(tr("Syntax error: %1").arg(message), true);
    }
}

void ScriptEditorPanel::onReload()
{
    const QString source = m_editor->toPlainText();
    const std::string chunk =
        m_isFile ? QFileInfo{m_filePath}.fileName().toStdString()
                 : (m_nodeId.isEmpty() ? "script.lua" : (m_nodeId.toStdString() + ".lua"));

    LuaRuntime runtime;
    (void)runtime.openLibraries();
    const Result syntaxResult = runtime.checkSyntax(source.toStdString(), chunk);
    if (syntaxResult.failed()) {
        const QString message = QString::fromStdString(std::string{syntaxResult.message()});
        const int line = errorLineOf(message);
        m_editor->setErrorLine(line);
        const QString errReport = tr("Syntax error: %1 (not reloaded)").arg(message);
        setStatus(errReport, true);
        Q_EMIT reported(errReport, true);
        return;
    }

    QString error;
    if (!commit(error)) {
        setStatus(error, true);
        Q_EMIT reported(error, true);
        return;
    }

    m_editor->setErrorLine(0);

    if (m_nodeId.isEmpty()) {
        setStatus(tr("✓ Standalone script saved and syntax validated."), false);
        return;
    }

    if (!m_running || m_library == nullptr) {
        setStatus(tr("✓ Saved & syntax validated. Will be deployed at next Start."), false);
        return;
    }

    m_library->offer(m_nodeId.toStdString(), source.toStdString());
    setStatus(tr("Hot reloading script into running ECU %1...").arg(m_nodeId), false);
}

void ScriptEditorPanel::onRevert()
{
    if (!isModified()) {
        return;
    }

    if (isVisible()) {
        const auto reply = QMessageBox::question(
            this,
            tr("Revert Script"),
            tr("Are you sure you want to discard all unsaved changes and revert to the last saved state?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);

        if (reply != QMessageBox::Yes) {
            return;
        }
    }

    const QSignalBlocker blocker{m_editor};
    m_editor->setPlainText(m_committed);
    m_editor->setErrorLine(0);

    updateDocumentHeader();
    updateStatusMetrics();
    updateAvailability();
    setStatus(tr("Reverted changes to last saved state."), false);
}

void ScriptEditorPanel::toggleFindReplace(bool visible)
{
    if (m_findWidget == nullptr) {
        return;
    }

    m_findWidget->setVisible(visible);
    if (m_actionFind != nullptr) {
        m_actionFind->setChecked(visible);
    }

    if (visible && m_findInput != nullptr) {
        const QTextCursor cursor = m_editor->textCursor();
        if (cursor.hasSelection()) {
            m_findInput->setText(cursor.selectedText());
        }
        m_findInput->setFocus();
        m_findInput->selectAll();
        onFindNext();
    } else if (!visible) {
        if (m_matchCountLabel != nullptr) {
            m_matchCountLabel->clear();
        }
        if (m_editor != nullptr) {
            m_editor->setFocus();
        }
    }
}

void ScriptEditorPanel::onFindNext()
{
    if (m_editor == nullptr || m_findInput == nullptr) {
        return;
    }

    const QString query = m_findInput->text();
    if (query.isEmpty()) {
        if (m_matchCountLabel != nullptr) {
            m_matchCountLabel->setText(QString{});
        }
        return;
    }

    const bool matchCase = m_matchCaseBtn != nullptr && m_matchCaseBtn->isChecked();
    const bool wholeWords = m_wholeWordsBtn != nullptr && m_wholeWordsBtn->isChecked();

    const bool found = m_editor->findText(query, matchCase, wholeWords, false);
    if (m_matchCountLabel != nullptr) {
        m_matchCountLabel->setText(found ? tr("Match found") : tr("Not found"));
    }
}

void ScriptEditorPanel::onFindPrevious()
{
    if (m_editor == nullptr || m_findInput == nullptr) {
        return;
    }

    const QString query = m_findInput->text();
    if (query.isEmpty()) {
        return;
    }

    const bool matchCase = m_matchCaseBtn != nullptr && m_matchCaseBtn->isChecked();
    const bool wholeWords = m_wholeWordsBtn != nullptr && m_wholeWordsBtn->isChecked();

    const bool found = m_editor->findText(query, matchCase, wholeWords, true);
    if (m_matchCountLabel != nullptr) {
        m_matchCountLabel->setText(found ? tr("Match found") : tr("Not found"));
    }
}

void ScriptEditorPanel::onReplace()
{
    if (m_editor == nullptr || m_findInput == nullptr || m_replaceInput == nullptr) {
        return;
    }

    const QString query = m_findInput->text();
    const QString rep = m_replaceInput->text();
    const bool matchCase = m_matchCaseBtn != nullptr && m_matchCaseBtn->isChecked();
    const bool wholeWords = m_wholeWordsBtn != nullptr && m_wholeWordsBtn->isChecked();

    m_editor->replaceCurrent(query, rep, matchCase, wholeWords);
}

void ScriptEditorPanel::onReplaceAll()
{
    if (m_editor == nullptr || m_findInput == nullptr || m_replaceInput == nullptr) {
        return;
    }

    const QString query = m_findInput->text();
    const QString rep = m_replaceInput->text();
    const bool matchCase = m_matchCaseBtn != nullptr && m_matchCaseBtn->isChecked();
    const bool wholeWords = m_wholeWordsBtn != nullptr && m_wholeWordsBtn->isChecked();

    const int count = m_editor->replaceAll(query, rep, matchCase, wholeWords);
    if (m_matchCountLabel != nullptr) {
        m_matchCountLabel->setText(tr("Replaced %1 occurrences").arg(count));
    }
    setStatus(tr("Replaced %1 occurrences of \"%2\"").arg(count).arg(query), false);
}

void ScriptEditorPanel::onTextChanged()
{
    if (m_editor->errorLine() > 0) {
        m_editor->setErrorLine(0);
    }

    updateDocumentHeader();
    updateStatusMetrics();
    updateAvailability();
}

void ScriptEditorPanel::onCursorPositionChanged()
{
    updateStatusMetrics();
}

void ScriptEditorPanel::updateStatusMetrics()
{
    if (m_editor == nullptr) {
        return;
    }

    if (m_cursorMetrics != nullptr) {
        m_cursorMetrics->setText(
            tr("Ln %1, Col %2").arg(m_editor->currentLine()).arg(m_editor->currentColumn()));
    }

    if (m_docMetrics != nullptr) {
        m_docMetrics->setText(
            tr("%1 lines  ·  %2 chars").arg(m_editor->lineCount()).arg(m_editor->characterCount()));
    }
}

void ScriptEditorPanel::refresh()
{
    if (m_library == nullptr) {
        return;
    }

    const std::vector<ScriptReload> reports = m_library->takeReports();

    for (const ScriptReload& report : reports) {
        const QString node = QString::fromStdString(report.nodeId);
        const QString message = QString::fromStdString(report.message);

        if (report.succeeded) {
            Q_EMIT reported(tr("%1: script reloaded").arg(node), false);
        } else {
            Q_EMIT reported(
                tr("%1: reload refused, running script is unchanged. %2").arg(node, message), true);
        }

        if (node != m_nodeId) {
            continue;
        }

        if (report.succeeded) {
            m_editor->setErrorLine(0);
            setStatus(tr("✓ Reloaded. The measurement did not stop."), false);
        } else {
            m_editor->setErrorLine(errorLineOf(message));
            setStatus(message, true);
        }
    }
}

int ScriptEditorPanel::errorLineOf(const QString& message)
{
    static const QRegularExpression pattern{QStringLiteral(":(\\d+):")};

    int line = 0;
    QRegularExpressionMatchIterator matches = pattern.globalMatch(message);

    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        bool parsed = false;
        const int candidate = match.captured(1).toInt(&parsed);
        if (parsed) {
            line = candidate;
            break;
        }
    }

    return line;
}

void ScriptEditorPanel::setStatus(const QString& text, bool isError)
{
    if (m_status == nullptr) {
        return;
    }

    m_status->setText(text);
    m_statusIsError = isError;

    const Theme theme = currentTheme();
    const QColor colour =
        isError ? theme.error : (text.startsWith(QChar(0x2713)) ? theme.success : theme.textMuted);

    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(colour.name()));
}

void ScriptEditorPanel::updateAvailability()
{
    if (m_editor == nullptr) {
        return;
    }

    if (m_actionUndo != nullptr) {
        m_actionUndo->setEnabled(m_editor->document()->isUndoAvailable());
    }
    if (m_actionRedo != nullptr) {
        m_actionRedo->setEnabled(m_editor->document()->isRedoAvailable());
    }
    if (m_actionSave != nullptr) {
        m_actionSave->setEnabled(isModified() || !m_filePath.isEmpty());
    }
    if (m_actionRevert != nullptr) {
        m_actionRevert->setEnabled(isModified());
    }
    if (m_actionReload != nullptr) {
        m_actionReload->setEnabled(!m_nodeId.isEmpty() || isModified());
        m_actionReload->setText(m_running && !m_nodeId.isEmpty() ? tr("Reload") : tr("Save"));
    }
}

void ScriptEditorPanel::onThemeChanged()
{
    applyIcons();

    if (m_editor != nullptr) {
        m_editor->applyTheme();
    }

    if (m_highlighter != nullptr) {
        m_highlighter->applyTheme();
    }

    if (m_editor != nullptr) {
        m_editor->setErrorLine(m_editor->errorLine());
    }

    updateDocumentHeader();
    setStatus(m_status->text(), m_statusIsError);
}

void ScriptEditorPanel::setDatabases(
    const std::vector<std::shared_ptr<const CanDatabase>>& databases)
{
    m_databases = databases;
    populateSignalsTree();
}

void ScriptEditorPanel::setVariables(SystemVariables* variables)
{
    m_variables = variables;
    populateVariablesTree();
}

void ScriptEditorPanel::toggleSidebar(bool visible)
{
    if (m_sidebarWidget == nullptr) {
        return;
    }

    if (!visible) {
        const QList<int> sizes = m_splitter != nullptr ? m_splitter->sizes() : QList<int>{};
        if (sizes.size() >= 2 && sizes[1] > 60) {
            m_lastSidebarWidth = sizes[1];
        }
        m_sidebarWidget->setVisible(false);
    } else {
        m_sidebarWidget->setVisible(true);
        if (m_splitter != nullptr) {
            const QList<int> sizes = m_splitter->sizes();
            if (sizes.size() >= 2) {
                const int total = sizes[0] + sizes[1];
                const int sideW = std::clamp(m_lastSidebarWidth, 200, std::max(200, total - 250));
                m_splitter->setSizes({total - sideW, sideW});
            }
        }
    }

    if (m_actionToggleSidebar != nullptr && m_actionToggleSidebar->isChecked() != visible) {
        const QSignalBlocker blocker(m_actionToggleSidebar);
        m_actionToggleSidebar->setChecked(visible);
    }
}

bool ScriptEditorPanel::isSidebarVisible() const
{
    return m_sidebarWidget != nullptr && !m_sidebarWidget->isHidden();
}

void ScriptEditorPanel::setupSidebar()
{
    m_sidebarWidget = new QWidget(m_splitter);
    m_sidebarWidget->setObjectName(QStringLiteral("scriptSidebar"));
    m_sidebarWidget->setMinimumWidth(240);

    auto* sidebarLayout = new QVBoxLayout(m_sidebarWidget);
    sidebarLayout->setContentsMargins(4, 4, 4, 4);
    sidebarLayout->setSpacing(4);

    ThemeManager* themes = ThemeManager::instance();
    const auto getIcon = [themes](const QString& name) {
        return themes != nullptr ? themes->icon(name) : QIcon{};
    };

    // 1. Sidebar Header
    auto* header = new QWidget(m_sidebarWidget);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(4, 2, 4, 2);
    headerLayout->setSpacing(6);

    auto* titleLabel = new QLabel(tr("CAPL / Lua Symbol Explorer"), header);
    titleLabel->setProperty("torquebusRole", QStringLiteral("caption"));
    QFont boldFont = titleLabel->font();
    boldFont.setBold(true);
    titleLabel->setFont(boldFont);
    headerLayout->addWidget(titleLabel, 1);

    auto* closeBtn = new QToolButton(header);
    closeBtn->setAutoRaise(true);
    closeBtn->setIcon(getIcon(QStringLiteral("close")));
    closeBtn->setToolTip(tr("Close Explorer (Ctrl+B)"));
    connect(closeBtn, &QToolButton::clicked, this, [this] { toggleSidebar(false); });
    headerLayout->addWidget(closeBtn);

    sidebarLayout->addWidget(header);

    // 2. Tab Widget
    m_sideTabs = new QTabWidget(m_sidebarWidget);
    m_sideTabs->setObjectName(QStringLiteral("scriptSidebarTabs"));

    // Tab 1: Signals
    auto* tabSignals = new QWidget(m_sideTabs);
    setupSignalsTab(tabSignals);
    m_sideTabs->addTab(tabSignals, getIcon(QStringLiteral("signal")), tr("Signals"));

    // Tab 2: Functions
    auto* tabFunctions = new QWidget(m_sideTabs);
    setupFunctionsTab(tabFunctions);
    m_sideTabs->addTab(tabFunctions, getIcon(QStringLiteral("script")), tr("Functions"));

    // Tab 3: Variables
    auto* tabVariables = new QWidget(m_sideTabs);
    setupVariablesTab(tabVariables);
    m_sideTabs->addTab(tabVariables, getIcon(QStringLiteral("gauge")), tr("Variables"));

    connect(m_sideTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) {
            onSignalItemSelectionChanged();
        } else if (index == 1) {
            onFunctionItemSelectionChanged();
        } else if (index == 2) {
            populateVariablesTree();
            onVariableItemSelectionChanged();
        }
    });

    sidebarLayout->addWidget(m_sideTabs, 1);

    // 3. Details & Insert Area
    auto* detailsBox = new QWidget(m_sidebarWidget);
    auto* detailsLayout = new QVBoxLayout(detailsBox);
    detailsLayout->setContentsMargins(2, 2, 2, 2);
    detailsLayout->setSpacing(2);

    auto* detailsHeaderLayout = new QHBoxLayout;
    detailsHeaderLayout->setContentsMargins(0, 0, 0, 0);
    m_detailsHeader = new QLabel(tr("Details & Documentation"), detailsBox);
    m_detailsHeader->setProperty("torquebusRole", QStringLiteral("caption"));
    detailsHeaderLayout->addWidget(m_detailsHeader, 1);

    m_insertSnippetBtn = new QPushButton(tr("Insert Snippet"), detailsBox);
    m_insertSnippetBtn->setProperty("torquebusRole", QStringLiteral("accent"));
    m_insertSnippetBtn->setIcon(getIcon(QStringLiteral("add")));
    m_insertSnippetBtn->setToolTip(tr("Insert selected symbol or template code into editor"));
    m_insertSnippetBtn->setEnabled(false);
    connect(
        m_insertSnippetBtn, &QPushButton::clicked, this, &ScriptEditorPanel::onInsertSelectedSnippet);
    detailsHeaderLayout->addWidget(m_insertSnippetBtn);

    detailsLayout->addLayout(detailsHeaderLayout);

    m_detailsText = new QTextEdit(detailsBox);
    m_detailsText->setReadOnly(true);
    m_detailsText->document()->setDocumentMargin(3);
    m_detailsText->setMaximumHeight(82);
    m_detailsText->setMinimumHeight(65);
    m_detailsText->setPlaceholderText(
        tr("Select a signal, function, or variable to inspect documentation and code snippets."));
    detailsLayout->addWidget(m_detailsText);

    sidebarLayout->addWidget(detailsBox);

    populateSignalsTree();
    populateFunctionsTree();
    populateVariablesTree();
}

void ScriptEditorPanel::setupSignalsTab(QWidget* parent)
{
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(2, 4, 2, 2);
    layout->setSpacing(4);

    ThemeManager* themes = ThemeManager::instance();

    m_signalFilterInput = new QLineEdit(parent);
    m_signalFilterInput->setPlaceholderText(tr("Filter signals or messages..."));
    m_signalFilterInput->setClearButtonEnabled(true);
    if (themes != nullptr) {
        m_signalFilterInput->addAction(themes->icon(QStringLiteral("search")),
                                       QLineEdit::LeadingPosition);
    }
    connect(m_signalFilterInput,
            &QLineEdit::textChanged,
            this,
            &ScriptEditorPanel::onSignalFilterChanged);
    layout->addWidget(m_signalFilterInput);

    m_signalsTree = new QTreeWidget(parent);
    m_signalsTree->setHeaderLabels({tr("Symbol"), tr("ID / Info")});
    m_signalsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_signalsTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_signalsTree->setAlternatingRowColors(true);
    m_signalsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_signalsTree->installEventFilter(this);

    connect(m_signalsTree,
            &QTreeWidget::itemDoubleClicked,
            this,
            &ScriptEditorPanel::onSignalItemDoubleClicked);
    connect(m_signalsTree,
            &QTreeWidget::itemSelectionChanged,
            this,
            &ScriptEditorPanel::onSignalItemSelectionChanged);

    connect(
        m_signalsTree,
        &QTreeWidget::customContextMenuRequested,
        this,
        [this](const QPoint& pos) {
            QTreeWidgetItem* item = m_signalsTree->itemAt(pos);
            if (item == nullptr) {
                return;
            }

            QMenu menu(this);
            const QString type = item->data(0, Qt::UserRole).toString();
            if (type == QStringLiteral("signal")) {
                const QString sigName = item->data(0, Qt::UserRole + 1).toString();
                menu.addAction(
                    tr("Insert emit_signal(\"%1\", value)").arg(sigName), this, [this, sigName] {
                        m_editor->insertSnippet(
                            QStringLiteral("emit_signal(\"%1\", 0.0)\n").arg(sigName));
                    });
                menu.addAction(
                    tr("Insert local %1 = signals[\"%1\"]").arg(sigName), this, [this, sigName] {
                        m_editor->insertSnippet(
                            QStringLiteral("local %1 = signals[\"%1\"]\n").arg(sigName));
                    });
                menu.addSeparator();
                menu.addAction(tr("Copy Signal Name"), this, [sigName] {
                    QGuiApplication::clipboard()->setText(sigName);
                });
            } else if (type == QStringLiteral("message")) {
                const quint32 msgId = item->data(0, Qt::UserRole + 2).toUInt();
                const QString idHex = QStringLiteral("0x%1").arg(msgId, 0, 16).toUpper();
                menu.addAction(tr("Insert emit(%1, payload)").arg(idHex), this, [this, idHex] {
                    m_editor->insertSnippet(
                        QStringLiteral("emit(%1, \"\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\")\n")
                            .arg(idHex));
                });
                menu.addAction(
                    tr("Insert cyclic(%1, 100, payload)").arg(idHex), this, [this, idHex] {
                        m_editor->insertSnippet(
                            QStringLiteral("cyclic(%1, 100, function()\n    return "
                                           "\"\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\"\nend)\n")
                                .arg(idHex));
                    });
                menu.addSeparator();
                menu.addAction(tr("Copy Message ID (%1)").arg(idHex), this, [idHex] {
                    QGuiApplication::clipboard()->setText(idHex);
                });
            }
            menu.exec(m_signalsTree->viewport()->mapToGlobal(pos));
        });

    layout->addWidget(m_signalsTree, 1);
}

void ScriptEditorPanel::setupFunctionsTab(QWidget* parent)
{
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(2, 4, 2, 2);
    layout->setSpacing(4);

    ThemeManager* themes = ThemeManager::instance();

    m_functionFilterInput = new QLineEdit(parent);
    m_functionFilterInput->setPlaceholderText(tr("Filter functions & handlers..."));
    m_functionFilterInput->setClearButtonEnabled(true);
    if (themes != nullptr) {
        m_functionFilterInput->addAction(themes->icon(QStringLiteral("search")),
                                         QLineEdit::LeadingPosition);
    }
    connect(m_functionFilterInput,
            &QLineEdit::textChanged,
            this,
            &ScriptEditorPanel::onFunctionFilterChanged);
    layout->addWidget(m_functionFilterInput);

    m_functionsTree = new QTreeWidget(parent);
    m_functionsTree->setHeaderLabels({tr("Function / Event"), tr("Syntax")});
    m_functionsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_functionsTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_functionsTree->setAlternatingRowColors(true);
    m_functionsTree->installEventFilter(this);

    connect(m_functionsTree,
            &QTreeWidget::itemDoubleClicked,
            this,
            &ScriptEditorPanel::onFunctionItemDoubleClicked);
    connect(m_functionsTree,
            &QTreeWidget::itemSelectionChanged,
            this,
            &ScriptEditorPanel::onFunctionItemSelectionChanged);

    layout->addWidget(m_functionsTree, 1);
}

void ScriptEditorPanel::setupVariablesTab(QWidget* parent)
{
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(2, 4, 2, 2);
    layout->setSpacing(4);

    ThemeManager* themes = ThemeManager::instance();

    m_variableFilterInput = new QLineEdit(parent);
    m_variableFilterInput->setPlaceholderText(tr("Filter variables & parameters..."));
    m_variableFilterInput->setClearButtonEnabled(true);
    if (themes != nullptr) {
        m_variableFilterInput->addAction(themes->icon(QStringLiteral("search")),
                                         QLineEdit::LeadingPosition);
    }
    connect(m_variableFilterInput,
            &QLineEdit::textChanged,
            this,
            &ScriptEditorPanel::onVariableFilterChanged);
    layout->addWidget(m_variableFilterInput);

    m_variablesTree = new QTreeWidget(parent);
    m_variablesTree->setHeaderLabels({tr("Variable"), tr("Value / Type")});
    m_variablesTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_variablesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_variablesTree->setAlternatingRowColors(true);
    m_variablesTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_variablesTree->installEventFilter(this);

    connect(m_variablesTree,
            &QTreeWidget::itemDoubleClicked,
            this,
            &ScriptEditorPanel::onVariableItemDoubleClicked);
    connect(m_variablesTree,
            &QTreeWidget::itemSelectionChanged,
            this,
            &ScriptEditorPanel::onVariableItemSelectionChanged);

    connect(
        m_variablesTree,
        &QTreeWidget::customContextMenuRequested,
        this,
        [this](const QPoint& pos) {
            QTreeWidgetItem* item = m_variablesTree->itemAt(pos);
            if (item == nullptr) {
                return;
            }

            const QString type = item->data(0, Qt::UserRole).toString();
            if (type == QStringLiteral("variable")) {
                const QString varName = item->data(0, Qt::UserRole + 1).toString();
                QMenu menu(this);
                menu.addAction(
                    tr("Insert var_get(\"%1\")").arg(varName), this, [this, varName] {
                        m_editor->insertSnippet(
                            QStringLiteral("local %1 = var_get(\"%1\")\n").arg(varName));
                    });
                menu.addAction(
                    tr("Insert var_set(\"%1\", value)").arg(varName), this, [this, varName] {
                        m_editor->insertSnippet(
                            QStringLiteral("var_set(\"%1\", 0.0)\n").arg(varName));
                    });
                menu.addSeparator();
                menu.addAction(tr("Copy Variable Name"), this, [varName] {
                    QGuiApplication::clipboard()->setText(varName);
                });
                menu.exec(m_variablesTree->viewport()->mapToGlobal(pos));
            }
        });

    layout->addWidget(m_variablesTree, 1);
}

void ScriptEditorPanel::populateSignalsTree()
{
    if (m_signalsTree == nullptr) {
        return;
    }

    m_signalsTree->clear();

    if (m_databases.empty()) {
        auto* placeholder = new QTreeWidgetItem(m_signalsTree);
        placeholder->setText(0, tr("No DBC Databases loaded"));
        placeholder->setText(1, tr("(Load via DBC Explorer)"));
        placeholder->setFlags(Qt::NoItemFlags);
        return;
    }

    ThemeManager* themes = ThemeManager::instance();
    const QIcon dbIcon = themes != nullptr ? themes->icon(QStringLiteral("database")) : QIcon{};
    const QIcon msgIcon = themes != nullptr ? themes->icon(QStringLiteral("message")) : QIcon{};
    const QIcon sigIcon = themes != nullptr ? themes->icon(QStringLiteral("signal")) : QIcon{};
    const QIcon sigMuxIcon =
        themes != nullptr ? themes->icon(QStringLiteral("signal-multiplex")) : sigIcon;

    for (const auto& db : m_databases) {
        if (!db) {
            continue;
        }

        const QString dbFileName = QFileInfo(QString::fromStdString(db->sourcePath)).fileName();
        auto* dbItem = new QTreeWidgetItem(m_signalsTree);
        dbItem->setIcon(0, dbIcon);
        dbItem->setText(0, dbFileName.isEmpty() ? tr("Database") : dbFileName);
        dbItem->setText(1, tr("%n msg", nullptr, static_cast<int>(db->messageCount())));
        dbItem->setData(0, Qt::UserRole, QStringLiteral("database"));
        dbItem->setData(0, Qt::UserRole + 1, QString::fromStdString(db->sourcePath));

        for (const auto& msg : db->messages()) {
            auto* msgItem = new QTreeWidgetItem(dbItem);
            msgItem->setIcon(0, msgIcon);
            msgItem->setText(0, QString::fromStdString(msg.name));
            const QString idHex = QStringLiteral("0x%1")
                                      .arg(msg.identifier,
                                           msg.format == CanFrameFormat::Extended ? 8 : 3,
                                           16,
                                           QLatin1Char('0'))
                                      .toUpper();
            msgItem->setText(1, idHex);
            msgItem->setData(0, Qt::UserRole, QStringLiteral("message"));
            msgItem->setData(0, Qt::UserRole + 1, QString::fromStdString(msg.name));
            msgItem->setData(0, Qt::UserRole + 2, static_cast<quint32>(msg.identifier));
            msgItem->setData(0, Qt::UserRole + 3, static_cast<int>(msg.length));
            msgItem->setData(0, Qt::UserRole + 4, static_cast<quint32>(msg.cycleTimeMs));
            msgItem->setData(0, Qt::UserRole + 5, QString::fromStdString(msg.transmitter));
            msgItem->setData(0, Qt::UserRole + 6, QString::fromStdString(msg.comment));

            for (const auto& sig : msg.signalList) {
                auto* sigItem = new QTreeWidgetItem(msgItem);
                sigItem->setIcon(0, sig.isMultiplexer ? sigMuxIcon : sigIcon);
                sigItem->setText(0, QString::fromStdString(sig.name));
                const QString unitStr = sig.unit.empty()
                                            ? QString()
                                            : QStringLiteral(" [%1]").arg(
                                                  QString::fromStdString(sig.unit));
                sigItem->setText(1, QStringLiteral("%1b%2").arg(sig.bitLength).arg(unitStr));

                sigItem->setData(0, Qt::UserRole, QStringLiteral("signal"));
                sigItem->setData(0, Qt::UserRole + 1, QString::fromStdString(sig.name));
                sigItem->setData(0, Qt::UserRole + 2, QString::fromStdString(msg.name));
                sigItem->setData(0, Qt::UserRole + 3, static_cast<quint32>(msg.identifier));
                sigItem->setData(0, Qt::UserRole + 4, static_cast<int>(sig.startBit));
                sigItem->setData(0, Qt::UserRole + 5, static_cast<int>(sig.bitLength));
                sigItem->setData(0,
                                 Qt::UserRole + 6,
                                 sig.byteOrder == ByteOrder::Intel ? QStringLiteral("Intel")
                                                                    : QStringLiteral("Motorola"));
                sigItem->setData(0, Qt::UserRole + 7, sig.factor);
                sigItem->setData(0, Qt::UserRole + 8, sig.offset);
                sigItem->setData(0, Qt::UserRole + 9, sig.minimum);
                sigItem->setData(0, Qt::UserRole + 10, sig.maximum);
                sigItem->setData(0, Qt::UserRole + 11, QString::fromStdString(sig.unit));
                sigItem->setData(0, Qt::UserRole + 12, QString::fromStdString(sig.comment));
            }
        }
        dbItem->setExpanded(true);
    }

    if (m_signalFilterInput != nullptr && !m_signalFilterInput->text().trimmed().isEmpty()) {
        onSignalFilterChanged(m_signalFilterInput->text());
    }
}

void ScriptEditorPanel::populateFunctionsTree()
{
    if (m_functionsTree == nullptr) {
        return;
    }

    m_functionsTree->clear();

    ThemeManager* themes = ThemeManager::instance();
    const QIcon funcIcon = themes != nullptr ? themes->icon(QStringLiteral("script")) : QIcon{};
    const QIcon eventIcon = themes != nullptr ? themes->icon(QStringLiteral("start")) : funcIcon;
    const QIcon txIcon = themes != nullptr ? themes->icon(QStringLiteral("transmit")) : funcIcon;
    const QIcon rxIcon = themes != nullptr ? themes->icon(QStringLiteral("trace")) : funcIcon;
    const QIcon timerIcon = themes != nullptr ? themes->icon(QStringLiteral("time-format")) : funcIcon;
    const QIcon varIcon = themes != nullptr ? themes->icon(QStringLiteral("gauge")) : funcIcon;
    const QIcon diagIcon = themes != nullptr ? themes->icon(QStringLiteral("diagnostics")) : funcIcon;
    const QIcon testIcon = themes != nullptr ? themes->icon(QStringLiteral("test")) : funcIcon;
    const QIcon utilIcon = themes != nullptr ? themes->icon(QStringLiteral("console")) : funcIcon;

    struct ApiEntry {
        QString category;
        QIcon catIcon;
        QString name;
        QString signature;
        QString description;
        QString snippet;
    };

    const std::vector<ApiEntry> entries = {
        // --- Event Handlers (Callbacks) ---
        {tr("Event Handlers"), eventIcon, QStringLiteral("on_start"), QStringLiteral("function on_start()"),
         tr("Called once when measurement begins or script is hot-reloaded. Ideal for initializing timers and state variables."),
         QStringLiteral("function on_start()\n    log_message(\"Simulation initialized\")\n    set_timer(50) -- 20 Hz\nend\n")},
        {tr("Event Handlers"), eventIcon, QStringLiteral("on_message"), QStringLiteral("function on_message(frame)"),
         tr("Called on every incoming CAN frame. Provides frame.id, frame.data, frame.channel, and frame.extended."),
         QStringLiteral("function on_message(frame)\n    if frame.id == 0x100 then\n        -- Process frame data\n    end\nend\n")},
        {tr("Event Handlers"), eventIcon, QStringLiteral("on_timer"), QStringLiteral("function on_timer(interval_ms)"),
         tr("Called periodically at the interval configured by set_timer(ms)."),
         QStringLiteral("function on_timer(interval_ms)\n    -- Periodic execution logic\nend\n")},
        {tr("Event Handlers"), eventIcon, QStringLiteral("on_stop"), QStringLiteral("function on_stop()"),
         tr("Called when measurement stops. Use for teardown and cleanup operations."),
         QStringLiteral("function on_stop()\n    log_message(\"Simulation stopped\")\nend\n")},
        {tr("Event Handlers"), eventIcon, QStringLiteral("on_security_seed"), QStringLiteral("function on_security_seed(seed)"),
         tr("Called when UDS SecurityAccess (0x27) request arrives. Return the expected unlock key."),
         QStringLiteral("function on_security_seed(seed)\n    -- Compute and return response key\n    return seed\nend\n")},
        {tr("Event Handlers"), eventIcon, QStringLiteral("on_uds_request"), QStringLiteral("function on_uds_request(request)"),
         tr("Custom UDS diagnostic request hook. Return response payload bytes, or nil to let the server handle it."),
         QStringLiteral("function on_uds_request(request)\n    -- Return response bytes or nil\n    return nil\nend\n")},

        // --- CAN Transmit & Cyclic ---
        {tr("CAN Transmit"), txIcon, QStringLiteral("emit"), QStringLiteral("emit(id, data, [options])"),
         tr("Transmits a raw CAN frame on this node's output port. Options: {channel = 0, extended = false}."),
         QStringLiteral("emit(0x100, \"\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\")\n")},
        {tr("CAN Transmit"), txIcon, QStringLiteral("emit_signal"), QStringLiteral("emit_signal(signal_name, value)"),
         tr("Packs physical value into its message according to loaded DBC and transmits the frame."),
         QStringLiteral("emit_signal(\"EngineSpeed\", 2500.0)\n")},
        {tr("CAN Transmit"), txIcon, QStringLiteral("cyclic"), QStringLiteral("cyclic(id, ms, payload_or_fn)"),
         tr("Registers autonomous periodic transmission of a frame at given interval in milliseconds."),
         QStringLiteral("cyclic(0x100, 50, function()\n    return \"\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\"\nend)\n")},
        {tr("CAN Transmit"), txIcon, QStringLiteral("stop_cyclic"), QStringLiteral("stop_cyclic(id)"),
         tr("Stops an active cyclic transmission previously registered with cyclic()."),
         QStringLiteral("stop_cyclic(0x100)\n")},
        {tr("CAN Transmit"), txIcon, QStringLiteral("fault"), QStringLiteral("fault(frame, fault_type)"),
         tr("Injects bus error or frame corruption for robustness testing."),
         QStringLiteral("fault(frame, \"crc\")\n")},

        // --- CAN Receive & Query ---
        {tr("CAN Receive"), rxIcon, QStringLiteral("decode"), QStringLiteral("decode(frame)"),
         tr("Decodes CAN frame payload into a dictionary of physical signal values using the loaded DBC."),
         QStringLiteral("local signals = decode(frame)\nif signals and signals.EngineSpeed then\n    local speed = signals.EngineSpeed\nend\n")},
        {tr("CAN Receive"), rxIcon, QStringLiteral("bus_last"), QStringLiteral("bus_last(id)"),
         tr("Retrieves the last frame observed on the bus for the given identifier."),
         QStringLiteral("local last = bus_last(0x100)\n")},
        {tr("CAN Receive"), rxIcon, QStringLiteral("bus_stats"), QStringLiteral("bus_stats()"),
         tr("Retrieves bus statistics including load percentage, frame counts, and errors."),
         QStringLiteral("local stats = bus_stats()\n")},

        // --- Timers & Clock ---
        {tr("Timers & Time"), timerIcon, QStringLiteral("set_timer"), QStringLiteral("set_timer(interval_ms)"),
         tr("Configures the recurrence period for the on_timer(interval_ms) callback."),
         QStringLiteral("set_timer(50) -- 20 Hz\n")},
        {tr("Timers & Time"), timerIcon, QStringLiteral("every"), QStringLiteral("every(interval_ms, fn)"),
         tr("Schedules an independent software timer callback executing at the given interval."),
         QStringLiteral("every(100, function()\n    -- 100 ms recurring task\nend)\n")},
        {tr("Timers & Time"), timerIcon, QStringLiteral("get_time_us"), QStringLiteral("get_time_us()"),
         tr("Returns measurement time in microseconds elapsed since Start."),
         QStringLiteral("local us = get_time_us()\n")},
        {tr("Timers & Time"), timerIcon, QStringLiteral("tb.now"), QStringLiteral("tb.now()"),
         tr("Returns measurement time in fractional seconds (float)."),
         QStringLiteral("local sec = tb.now()\n")},

        // --- System Variables ---
        {tr("System Variables"), varIcon, QStringLiteral("var_get"), QStringLiteral("var_get(name)"),
         tr("Reads atomic value of a system variable shared with Dashboard gauges, sliders, and nodes."),
         QStringLiteral("local value = var_get(\"VariableName\")\n")},
        {tr("System Variables"), varIcon, QStringLiteral("var_set"), QStringLiteral("var_set(name, value)"),
         tr("Writes atomic value to a system variable shared across the project."),
         QStringLiteral("var_set(\"VariableName\", 100.0)\n")},

        // --- Diagnostics (UDS) ---
        {tr("Diagnostics (UDS)"), diagIcon, QStringLiteral("uds_did"), QStringLiteral("uds_did(did, value, [options])"),
         tr("Registers or updates a UDS Data Identifier (DID) for simulated ECU diagnostics."),
         QStringLiteral("uds_did(0xF190, \"WVWZZZ1KZ9W123456\")\n")},
        {tr("Diagnostics (UDS)"), diagIcon, QStringLiteral("uds_dtc"), QStringLiteral("uds_dtc(dtc, status)"),
         tr("Registers or updates a Diagnostic Trouble Code (DTC) in simulated fault memory."),
         QStringLiteral("uds_dtc(0x012800, 0x2F)\n")},
        {tr("Diagnostics (UDS)"), diagIcon, QStringLiteral("uds_clear_dtc"), QStringLiteral("uds_clear_dtc()"),
         tr("Clears all active DTC faults in ECU diagnostic memory."),
         QStringLiteral("uds_clear_dtc()\n")},
        {tr("Diagnostics (UDS)"), diagIcon, QStringLiteral("uds_session"), QStringLiteral("uds_session()"),
         tr("Returns current diagnostic session ID (Default, Extended, Programming)."),
         QStringLiteral("local session = uds_session()\n")},

        // --- Signal Generators (tb.*) ---
        {tr("Signal Generators"), funcIcon, QStringLiteral("tb.ramp"), QStringLiteral("tb.ramp(low, high, period)"),
         tr("Returns a generator function sweeping linearly from low to high over period seconds."),
         QStringLiteral("local gen_ramp = tb.ramp(0, 100, 10)\n")},
        {tr("Signal Generators"), funcIcon, QStringLiteral("tb.sine"), QStringLiteral("tb.sine(low, high, period)"),
         tr("Returns a generator function oscillating sinusoidally between low and high."),
         QStringLiteral("local gen_sine = tb.sine(800, 6000, 5)\n")},
        {tr("Signal Generators"), funcIcon, QStringLiteral("tb.square"), QStringLiteral("tb.square(low, high, period)"),
         tr("Returns a generator function alternating between low and high 50% duty cycle."),
         QStringLiteral("local gen_square = tb.square(0, 1, 2)\n")},
        {tr("Signal Generators"), funcIcon, QStringLiteral("tb.drift"), QStringLiteral("tb.drift(low, high, step)"),
         tr("Returns a generator simulating realistic sensor random drift noise within bounds."),
         QStringLiteral("local gen_drift = tb.drift(19.0, 25.0, 0.2)\n")},
        {tr("Signal Generators"), funcIcon, QStringLiteral("tb.counter"), QStringLiteral("tb.counter(bits)"),
         tr("Returns a rolling rollover counter generator (e.g. 4 bits = 0..15)."),
         QStringLiteral("local roll_cnt = tb.counter(4)\n")},
        {tr("Signal Generators"), funcIcon, QStringLiteral("tb.e2e"), QStringLiteral("tb.e2e(payload, counter)"),
         tr("Builds AUTOSAR End-to-End Profile 1 payload with CRC-8 and rolling alive counter."),
         QStringLiteral("local body = tb.e2e(payload, roll_cnt())\n")},

        // --- Testing Framework ---
        {tr("Test Framework"), testIcon, QStringLiteral("test"), QStringLiteral("test(name, fn)"),
         tr("Declares an asynchronous test case executed in a coroutine sequence."),
         QStringLiteral("test(\"Verify engine idle speed\", function()\n    wait(100)\n    assert_between(signals.EngineSpeed or 0, 750, 850, \"Idle speed in range\")\nend)\n")},
        {tr("Test Framework"), testIcon, QStringLiteral("wait"), QStringLiteral("wait(milliseconds)"),
         tr("Yields test coroutine for the specified delay while measurement continues."),
         QStringLiteral("wait(100)\n")},
        {tr("Test Framework"), testIcon, QStringLiteral("expect"), QStringLiteral("expect(id, [options])"),
         tr("Awaits a CAN frame with timeout (within=ms) and optional predicate (where=fn)."),
         QStringLiteral("local frame = expect(0x100, { within = 500 })\nassert_true(frame ~= nil, \"Frame 0x100 received\")\n")},
        {tr("Test Framework"), testIcon, QStringLiteral("assert_true"), QStringLiteral("assert_true(cond, desc)"),
         tr("Verifies condition is true; records check in test report and fails case if false."),
         QStringLiteral("assert_true(speed > 0, \"Vehicle is moving\")\n")},
        {tr("Test Framework"), testIcon, QStringLiteral("assert_equal"), QStringLiteral("assert_equal(act, exp, desc)"),
         tr("Verifies actual value equals expected value."),
         QStringLiteral("assert_equal(gear, 1, \"Gear is first\")\n")},
        {tr("Test Framework"), testIcon, QStringLiteral("assert_near"), QStringLiteral("assert_near(act, exp, tol, desc)"),
         tr("Verifies floating-point value is within tolerance of expected value."),
         QStringLiteral("assert_near(voltage, 12.6, 0.2, \"Battery voltage normal\")\n")},
        {tr("Test Framework"), testIcon, QStringLiteral("assert_between"), QStringLiteral("assert_between(act, low, high, desc)"),
         tr("Verifies actual value falls between low and high limits."),
         QStringLiteral("assert_between(temp, 85.0, 95.0, \"Coolant temp in range\")\n")},

        // --- Output & Logging ---
        {tr("Output & Logging"), utilIcon, QStringLiteral("log_message"), QStringLiteral("log_message(text)"),
         tr("Prints informational line to the TorqueBus Studio Output panel."),
         QStringLiteral("log_message(\"Checkpoint reached\")\n")},
        {tr("Output & Logging"), utilIcon, QStringLiteral("print"), QStringLiteral("print(...)"),
         tr("Standard Lua output printed to log."),
         QStringLiteral("print(\"Value: \" .. tostring(val))\n")},
    };

    std::map<QString, QTreeWidgetItem*> categoryNodes;

    for (const auto& entry : entries) {
        auto it = categoryNodes.find(entry.category);
        QTreeWidgetItem* catItem = nullptr;
        if (it == categoryNodes.end()) {
            catItem = new QTreeWidgetItem(m_functionsTree);
            catItem->setIcon(0, entry.catIcon);
            catItem->setText(0, entry.category);
            catItem->setData(0, Qt::UserRole, QStringLiteral("category"));
            categoryNodes[entry.category] = catItem;
            catItem->setExpanded(true);
        } else {
            catItem = it->second;
        }

        auto* item = new QTreeWidgetItem(catItem);
        item->setIcon(0, funcIcon);
        item->setText(0, entry.name);
        item->setText(1, entry.signature);
        item->setData(0, Qt::UserRole, QStringLiteral("function"));
        item->setData(0, Qt::UserRole + 1, entry.name);
        item->setData(0, Qt::UserRole + 2, entry.signature);
        item->setData(0, Qt::UserRole + 3, entry.description);
        item->setData(0, Qt::UserRole + 4, entry.snippet);
    }

    if (m_functionFilterInput != nullptr && !m_functionFilterInput->text().trimmed().isEmpty()) {
        onFunctionFilterChanged(m_functionFilterInput->text());
    }
}

void ScriptEditorPanel::populateVariablesTree()
{
    if (m_variablesTree == nullptr) {
        return;
    }

    m_variablesTree->clear();

    ThemeManager* themes = ThemeManager::instance();
    const QIcon varIcon = themes != nullptr ? themes->icon(QStringLiteral("gauge")) : QIcon{};
    const QIcon paramIcon = themes != nullptr ? themes->icon(QStringLiteral("properties")) : varIcon;

    // 1. System Variables Group
    auto* sysGroup = new QTreeWidgetItem(m_variablesTree);
    sysGroup->setIcon(0, varIcon);
    sysGroup->setText(0, tr("System Variables"));
    sysGroup->setData(0, Qt::UserRole, QStringLiteral("category"));

    if (m_variables != nullptr && m_variables->count() > 0) {
        const auto names = m_variables->names();
        sysGroup->setText(1, tr("%n var(s)", nullptr, static_cast<int>(names.size())));
        for (const auto& name : names) {
            auto* item = new QTreeWidgetItem(sysGroup);
            item->setIcon(0, varIcon);
            const QString qName = QString::fromStdString(name);
            item->setText(0, qName);
            item->setText(1, QString::number(m_variables->value(name)));
            item->setData(0, Qt::UserRole, QStringLiteral("variable"));
            item->setData(0, Qt::UserRole + 1, qName);
            item->setData(0, Qt::UserRole + 2, m_variables->value(name));
        }
    } else {
        sysGroup->setText(1, tr("(0 variables)"));
        auto* hint = new QTreeWidgetItem(sysGroup);
        hint->setText(0, tr("No System Variables yet"));
        hint->setText(1, tr("var_set(\"name\", val)"));
        hint->setFlags(Qt::NoItemFlags);
    }
    sysGroup->setExpanded(true);

    // 2. Node Parameters Group
    auto* paramGroup = new QTreeWidgetItem(m_variablesTree);
    paramGroup->setIcon(0, paramIcon);
    paramGroup->setText(0, tr("Node Parameters"));
    paramGroup->setText(1, tr("Read-only context"));
    paramGroup->setData(0, Qt::UserRole, QStringLiteral("category"));

    auto addParam = [&](const QString& name, const QString& type, const QString& desc, const QString& snippet) {
        auto* item = new QTreeWidgetItem(paramGroup);
        item->setIcon(0, paramIcon);
        item->setText(0, name);
        item->setText(1, type);
        item->setData(0, Qt::UserRole, QStringLiteral("parameter"));
        item->setData(0, Qt::UserRole + 1, snippet);
        item->setData(0, Qt::UserRole + 2, desc);
    };

    addParam(QStringLiteral("channel"), tr("Integer (0..N)"), tr("CAN Channel this ECU transmits and receives on."), QStringLiteral("channel"));
    addParam(QStringLiteral("parameters.channel"), tr("Integer"), tr("Node parameter for CAN channel."), QStringLiteral("parameters.channel"));
    addParam(QStringLiteral("parameters.scriptPath"), tr("String"), tr("Path to the loaded external script file."), QStringLiteral("parameters.scriptPath"));
    addParam(QStringLiteral("descriptionId"), tr("String"), tr("Unique node ID in the pipeline graph."), QStringLiteral("\"%1\"").arg(m_nodeId));

    paramGroup->setExpanded(true);

    if (m_variableFilterInput != nullptr && !m_variableFilterInput->text().trimmed().isEmpty()) {
        onVariableFilterChanged(m_variableFilterInput->text());
    }
}

void ScriptEditorPanel::onSignalFilterChanged(const QString& filter)
{
    if (m_signalsTree == nullptr) {
        return;
    }
    for (int i = 0; i < m_signalsTree->topLevelItemCount(); ++i) {
        applyTreeFilter(m_signalsTree->topLevelItem(i), filter.trimmed());
    }
}

void ScriptEditorPanel::onFunctionFilterChanged(const QString& filter)
{
    if (m_functionsTree == nullptr) {
        return;
    }
    for (int i = 0; i < m_functionsTree->topLevelItemCount(); ++i) {
        applyTreeFilter(m_functionsTree->topLevelItem(i), filter.trimmed());
    }
}

void ScriptEditorPanel::onVariableFilterChanged(const QString& filter)
{
    if (m_variablesTree == nullptr) {
        return;
    }
    for (int i = 0; i < m_variablesTree->topLevelItemCount(); ++i) {
        applyTreeFilter(m_variablesTree->topLevelItem(i), filter.trimmed());
    }
}

bool ScriptEditorPanel::applyTreeFilter(QTreeWidgetItem* item, const QString& needle)
{
    if (item == nullptr) {
        return false;
    }

    bool anyChildVisible = false;
    for (int i = 0; i < item->childCount(); ++i) {
        if (applyTreeFilter(item->child(i), needle)) {
            anyChildVisible = true;
        }
    }

    const bool matches = needle.isEmpty()
                         || item->text(0).contains(needle, Qt::CaseInsensitive)
                         || item->text(1).contains(needle, Qt::CaseInsensitive);

    const bool visible = matches || anyChildVisible;
    item->setHidden(!visible);
    if (visible && !needle.isEmpty()) {
        item->setExpanded(true);
    }
    return visible;
}

void ScriptEditorPanel::onSignalItemSelectionChanged()
{
    if (m_signalsTree == nullptr || m_detailsText == nullptr) {
        return;
    }

    QTreeWidgetItem* item = m_signalsTree->currentItem();
    if (item == nullptr) {
        m_detailsText->clear();
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(false);
        }
        return;
    }

    const QString type = item->data(0, Qt::UserRole).toString();
    if (type == QStringLiteral("signal")) {
        const QString sigName = item->data(0, Qt::UserRole + 1).toString();
        const QString msgName = item->data(0, Qt::UserRole + 2).toString();
        const quint32 msgId = item->data(0, Qt::UserRole + 3).toUInt();
        const int startBit = item->data(0, Qt::UserRole + 4).toInt();
        const int bitLen = item->data(0, Qt::UserRole + 5).toInt();
        const QString endian = item->data(0, Qt::UserRole + 6).toString();
        const double factor = item->data(0, Qt::UserRole + 7).toDouble();
        const double offset = item->data(0, Qt::UserRole + 8).toDouble();
        const double min = item->data(0, Qt::UserRole + 9).toDouble();
        const double max = item->data(0, Qt::UserRole + 10).toDouble();
        const QString unit = item->data(0, Qt::UserRole + 11).toString();
        const QString comment = item->data(0, Qt::UserRole + 12).toString();

        QString html = QStringLiteral("<b>Signal:</b> %1<br/>"
                                      "<b>Message:</b> %2 (0x%3)<br/>"
                                      "<b>Bit Layout:</b> start %4, length %5 (%6)<br/>"
                                      "<b>Scale / Offset:</b> %7 / %8<br/>"
                                      "<b>Range:</b> %9 .. %10 %11")
                           .arg(sigName,
                                msgName,
                                QString::number(msgId, 16).toUpper(),
                                QString::number(startBit),
                                QString::number(bitLen),
                                endian,
                                QString::number(factor),
                                QString::number(offset),
                                QString::number(min),
                                QString::number(max),
                                unit);
        if (!comment.isEmpty()) {
            html += QStringLiteral("<br/><b>Comment:</b> %1").arg(comment.toHtmlEscaped());
        }
        m_detailsText->setHtml(html);
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(true);
        }
    } else if (type == QStringLiteral("message")) {
        const QString msgName = item->data(0, Qt::UserRole + 1).toString();
        const quint32 msgId = item->data(0, Qt::UserRole + 2).toUInt();
        const int dlc = item->data(0, Qt::UserRole + 3).toInt();
        const quint32 cycleTime = item->data(0, Qt::UserRole + 4).toUInt();
        const QString transmitter = item->data(0, Qt::UserRole + 5).toString();
        const QString comment = item->data(0, Qt::UserRole + 6).toString();

        QString html = QStringLiteral("<b>Message:</b> %1 (0x%2)<br/>"
                                      "<b>Payload Length:</b> %3 bytes<br/>"
                                      "<b>Transmitter:</b> %4<br/>"
                                      "<b>Cycle Time:</b> %5 ms")
                           .arg(msgName,
                                QString::number(msgId, 16).toUpper(),
                                QString::number(dlc),
                                transmitter.isEmpty() ? tr("unspecified") : transmitter,
                                QString::number(cycleTime));
        if (!comment.isEmpty()) {
            html += QStringLiteral("<br/><b>Comment:</b> %1").arg(comment.toHtmlEscaped());
        }
        m_detailsText->setHtml(html);
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(true);
        }
    } else {
        m_detailsText->clear();
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(false);
        }
    }
}

void ScriptEditorPanel::onFunctionItemSelectionChanged()
{
    if (m_functionsTree == nullptr || m_detailsText == nullptr) {
        return;
    }

    QTreeWidgetItem* item = m_functionsTree->currentItem();
    if (item == nullptr || item->data(0, Qt::UserRole).toString() != QStringLiteral("function")) {
        m_detailsText->clear();
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(false);
        }
        return;
    }

    const QString name = item->data(0, Qt::UserRole + 1).toString();
    const QString signature = item->data(0, Qt::UserRole + 2).toString();
    const QString description = item->data(0, Qt::UserRole + 3).toString();
    const QString snippet = item->data(0, Qt::UserRole + 4).toString();

    const QString html = QStringLiteral("<b><code>%1</code></b><br/>"
                                        "<i>%2</i><br/><br/>"
                                        "<b>Snippet:</b><br/><code>%3</code>")
                             .arg(signature.toHtmlEscaped(),
                                  description.toHtmlEscaped(),
                                  snippet.toHtmlEscaped());
    m_detailsText->setHtml(html);
    if (m_insertSnippetBtn != nullptr) {
        m_insertSnippetBtn->setEnabled(true);
    }
}

void ScriptEditorPanel::onVariableItemSelectionChanged()
{
    if (m_variablesTree == nullptr || m_detailsText == nullptr) {
        return;
    }

    QTreeWidgetItem* item = m_variablesTree->currentItem();
    if (item == nullptr) {
        m_detailsText->clear();
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(false);
        }
        return;
    }

    const QString type = item->data(0, Qt::UserRole).toString();
    if (type == QStringLiteral("variable")) {
        const QString varName = item->data(0, Qt::UserRole + 1).toString();
        const double val = item->data(0, Qt::UserRole + 2).toDouble();

        const QString html =
            QStringLiteral("<b>System Variable:</b> %1<br/>"
                           "<b>Current Value:</b> %2<br/><br/>"
                           "Shared atomic double value across script, dashboard, and pipeline.<br/>"
                           "<b>Example:</b><br/><code>local val = var_get(\"%1\")</code>")
                .arg(varName, QString::number(val));
        m_detailsText->setHtml(html);
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(true);
        }
    } else if (type == QStringLiteral("parameter")) {
        const QString snippet = item->data(0, Qt::UserRole + 1).toString();
        const QString desc = item->data(0, Qt::UserRole + 2).toString();

        const QString html = QStringLiteral("<b>Parameter:</b> %1<br/>"
                                            "<i>%2</i>")
                                 .arg(snippet.toHtmlEscaped(), desc.toHtmlEscaped());
        m_detailsText->setHtml(html);
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(true);
        }
    } else {
        m_detailsText->clear();
        if (m_insertSnippetBtn != nullptr) {
            m_insertSnippetBtn->setEnabled(false);
        }
    }
}

void ScriptEditorPanel::onSignalItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (item == nullptr) {
        return;
    }
    const QString type = item->data(0, Qt::UserRole).toString();
    if (type == QStringLiteral("signal")) {
        const QString sigName = item->data(0, Qt::UserRole + 1).toString();
        m_editor->insertSnippet(QStringLiteral("emit_signal(\"%1\", 0.0)\n").arg(sigName));
    } else if (type == QStringLiteral("message")) {
        const quint32 msgId = item->data(0, Qt::UserRole + 2).toUInt();
        m_editor->insertSnippet(
            QStringLiteral("emit(0x%1, \"\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\")\n")
                .arg(QString::number(msgId, 16).toUpper()));
    }
}

void ScriptEditorPanel::onFunctionItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (item == nullptr || item->data(0, Qt::UserRole).toString() != QStringLiteral("function")) {
        return;
    }
    const QString snippet = item->data(0, Qt::UserRole + 4).toString();
    if (!snippet.isEmpty()) {
        m_editor->insertSnippet(
            snippet.endsWith(QLatin1Char('\n')) ? snippet : snippet + QLatin1Char('\n'));
    }
}

void ScriptEditorPanel::onVariableItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (item == nullptr) {
        return;
    }
    const QString type = item->data(0, Qt::UserRole).toString();
    if (type == QStringLiteral("variable")) {
        const QString varName = item->data(0, Qt::UserRole + 1).toString();
        m_editor->insertSnippet(QStringLiteral("local %1 = var_get(\"%1\")\n").arg(varName));
    } else if (type == QStringLiteral("parameter")) {
        const QString snippet = item->data(0, Qt::UserRole + 1).toString();
        m_editor->insertSnippet(snippet);
    }
}

void ScriptEditorPanel::onInsertSelectedSnippet()
{
    const int currentTab = m_sideTabs != nullptr ? m_sideTabs->currentIndex() : 0;
    QTreeWidgetItem* item = nullptr;
    if (currentTab == 0 && m_signalsTree != nullptr) {
        item = m_signalsTree->currentItem();
    } else if (currentTab == 1 && m_functionsTree != nullptr) {
        item = m_functionsTree->currentItem();
    } else if (currentTab == 2 && m_variablesTree != nullptr) {
        item = m_variablesTree->currentItem();
    }

    if (item == nullptr) {
        return;
    }

    const QString type = item->data(0, Qt::UserRole).toString();
    if (type == QStringLiteral("signal")) {
        const QString sigName = item->data(0, Qt::UserRole + 1).toString();
        m_editor->insertSnippet(QStringLiteral("emit_signal(\"%1\", 0.0)\n").arg(sigName));
    } else if (type == QStringLiteral("message")) {
        const quint32 msgId = item->data(0, Qt::UserRole + 2).toUInt();
        const QString idHex = QStringLiteral("0x%1").arg(msgId, 0, 16).toUpper();
        m_editor->insertSnippet(
            QStringLiteral("emit(%1, \"\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\")\n").arg(idHex));
    } else if (type == QStringLiteral("function")) {
        const QString snippet = item->data(0, Qt::UserRole + 4).toString();
        if (!snippet.isEmpty()) {
            m_editor->insertSnippet(
                snippet.endsWith(QLatin1Char('\n')) ? snippet : snippet + QLatin1Char('\n'));
        }
    } else if (type == QStringLiteral("variable")) {
        const QString varName = item->data(0, Qt::UserRole + 1).toString();
        m_editor->insertSnippet(QStringLiteral("local %1 = var_get(\"%1\")\n").arg(varName));
    } else if (type == QStringLiteral("parameter")) {
        const QString snippet = item->data(0, Qt::UserRole + 1).toString();
        m_editor->insertSnippet(snippet);
    }
}

bool ScriptEditorPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (watched == m_signalsTree && m_signalsTree != nullptr) {
                if (auto* item = m_signalsTree->currentItem()) {
                    onSignalItemDoubleClicked(item, 0);
                    return true;
                }
            } else if (watched == m_functionsTree && m_functionsTree != nullptr) {
                if (auto* item = m_functionsTree->currentItem()) {
                    onFunctionItemDoubleClicked(item, 0);
                    return true;
                }
            } else if (watched == m_variablesTree && m_variablesTree != nullptr) {
                if (auto* item = m_variablesTree->currentItem()) {
                    onVariableItemDoubleClicked(item, 0);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace torquebus::ui
