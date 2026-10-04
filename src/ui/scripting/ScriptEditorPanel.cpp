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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <vector>

namespace torquebus::ui {
namespace {

/// 20 Hz, like every other panel that reads shared state on a timer.
constexpr int kRefreshMs = 50;

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

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
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    m_placeholder = new QLabel(tr("Select a Lua ECU block to edit its script."));
    m_placeholder->setProperty("torquebusRole", QStringLiteral("caption"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    layout->addWidget(m_placeholder, 1);

    m_body = new QWidget;
    auto* body = new QVBoxLayout(m_body);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(6);

    // --- Which script, and where it is kept -------------------------------
    m_header = new QLabel;
    m_header->setProperty("torquebusRole", QStringLiteral("caption"));
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(m_header);

    m_editor = new ScriptEdit;
    m_highlighter = new LuaHighlighter(m_editor->document());

    connect(m_editor, &ScriptEdit::textChanged, this, &ScriptEditorPanel::onTextChanged);

    body->addWidget(m_editor, 1);

    // --- What just happened, and what to do about it ----------------------
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    m_status = new QLabel;
    m_status->setProperty("torquebusRole", QStringLiteral("caption"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);
    row->addWidget(m_status, 1);

    m_revert = new QPushButton(tr("Revert"));
    m_revert->setToolTip(tr("Goes back to the script as it was last saved."));
    connect(m_revert, &QPushButton::clicked, this, &ScriptEditorPanel::onRevert);
    row->addWidget(m_revert);

    m_check = new QPushButton(tr("Check Syntax"));
    m_check->setToolTip(tr("Checks the Lua script syntax without running it (F7)."));
    m_check->setShortcut(QKeySequence{Qt::Key_F7});
    connect(m_check, &QPushButton::clicked, this, &ScriptEditorPanel::onCheckSyntax);
    row->addWidget(m_check);

    m_reload = new QPushButton(tr("Reload"));
    m_reload->setDefault(true);
    m_reload->setShortcut(QKeySequence{Qt::Key_F5});
    m_reload->setToolTip(tr("Saves the script and hands it to the running ECU. The "
                            "measurement keeps running; if the script does not load, "
                            "the one on the bus is left exactly as it is."));
    connect(m_reload, &QPushButton::clicked, this, &ScriptEditorPanel::onReload);
    row->addWidget(m_reload);

    applyIcons();

    body->addLayout(row);

    layout->addWidget(m_body, 1);

    m_body->setVisible(false);
}

void ScriptEditorPanel::applyIcons()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr) {
        return;
    }

    if (m_check != nullptr) {
        m_check->setIcon(themes->icon(QStringLiteral("check")));
        m_check->setIconSize(QSize(14, 14));
    }
    if (m_reload != nullptr) {
        m_reload->setIcon(themes->icon(QStringLiteral("replay")));
        m_reload->setIconSize(QSize(14, 14));
    }
    if (m_revert != nullptr) {
        m_revert->setIcon(themes->icon(QStringLiteral("skip-back")));
        m_revert->setIconSize(QSize(14, 14));
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
    return m_editor != nullptr && !m_nodeId.isEmpty() && m_editor->toPlainText() != m_committed;
}

void ScriptEditorPanel::clear()
{
    m_nodeId.clear();
    m_committed.clear();
    m_isFile = false;
    m_filePath.clear();

    if (m_editor != nullptr) {
        const QSignalBlocker blocker{m_editor};
        m_editor->clear();
        m_editor->setErrorLine(0);
    }

    m_body->setVisible(false);
    m_placeholder->setVisible(true);
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

    m_header->setText(m_isFile ? tr("%1  ·  %2").arg(m_nodeId, QFileInfo{m_filePath}.fileName())
                               : tr("%1  ·  script kept in the project").arg(m_nodeId));

    m_header->setToolTip(m_isFile ? m_filePath : QString{});

    m_placeholder->setVisible(false);
    m_body->setVisible(true);

    setStatus(QString{}, false);
    updateAvailability();
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

        // Relative to the project, like every other path in a .tbsproj - a
        // project that opens from its own folder and from nowhere else is not a
        // property anybody would guess a project file had.
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

    if (m_isFile) {
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
    } else {
        NodeDescription* node =
            const_cast<NodeDescription*>(m_description.find(m_nodeId.toStdString()));

        if (node == nullptr) {
            error = tr("The block %1 is no longer on the canvas.").arg(m_nodeId);
            return false;
        }

        node->parameters.set("script", ParameterValue::fromText(source.toStdString()));
        Q_EMIT nodeEdited(m_nodeId);
    }

    m_committed = source;
    updateAvailability();

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
    if (m_nodeId.isEmpty()) {
        return;
    }

    // Validate Lua syntax before committing or offering reload
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

    // Saved first, always. A reload that took effect on the bus but left the
    // project holding the old text would be undone by the next Start, and the
    // hour spent finding out why would be entirely this panel's fault.
    if (!commit(error)) {
        setStatus(error, true);
        Q_EMIT reported(error, true);
        return;
    }

    m_editor->setErrorLine(0);

    if (!m_running || m_library == nullptr) {
        setStatus(tr("✓ Saved & syntax validated. Will be used at next Start."), false);
        return;
    }

    m_library->offer(m_nodeId.toStdString(), m_editor->toPlainText().toStdString());

    // Not "reloaded": the node takes the offer on its next pass, and whether it
    // loaded is a question only the node can answer. Saying so now and being
    // contradicted 50 ms later is how a status line stops being believed.
    setStatus(tr("Reloading..."), false);
}

void ScriptEditorPanel::onRevert()
{
    const QSignalBlocker blocker{m_editor};

    m_editor->setPlainText(m_committed);
    m_editor->setErrorLine(0);

    setStatus(QString{}, false);
    updateAvailability();
}

void ScriptEditorPanel::onTextChanged()
{
    // The mark belongs to the text that produced it. Once that text has been
    // edited, a red line 42 is pointing at something that may no longer be
    // there.
    if (m_editor->errorLine() > 0) {
        m_editor->setErrorLine(0);
    }

    updateAvailability();
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

        // Every outcome goes to the Output panel, whichever node it is about:
        // this panel shows one script at a time, and a reload of another one
        // must not vanish because somebody clicked elsewhere.
        if (report.succeeded) {
            Q_EMIT reported(tr("%1: script reloaded").arg(node), false);
        } else {
            Q_EMIT reported(
                tr("%1: reload refused, the running script is unchanged. %2").arg(node, message),
                true);
        }

        if (node != m_nodeId) {
            continue;
        }

        if (report.succeeded) {
            m_editor->setErrorLine(0);
            setStatus(tr("Reloaded. The measurement did not stop."), false);
        } else {
            m_editor->setErrorLine(errorLineOf(message));
            setStatus(message, true);
        }
    }
}

int ScriptEditorPanel::errorLineOf(const QString& message)
{
    // Lua writes `chunkname:42: what went wrong`, and the node prefixes the
    // script's name - so the *last* `:digits:` before the message is the line,
    // and a Windows path's `C:` at the front is not mistaken for one.
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
    if (m_reload == nullptr) {
        return;
    }

    const bool hasNode = !m_nodeId.isEmpty();

    if (m_check != nullptr) {
        m_check->setEnabled(hasNode);
    }
    m_reload->setEnabled(hasNode);
    m_revert->setEnabled(hasNode && isModified());

    // The button says which of the two things it will do, because they are
    // different things: one changes an ECU that is on the bus right now.
    m_reload->setText(m_running ? tr("Reload") : tr("Save"));
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

    // Repainted in the colour it was already in - a refused reload does not
    // become good news because somebody switched to the light theme.
    setStatus(m_status->text(), m_statusIsError);
}

} // namespace torquebus::ui
