// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/scripting/ScriptEdit.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTextBlock>
#include <QWheelEvent>

namespace torquebus::ui {
namespace {

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

} // namespace

// ---------------------------------------------------------------------------
// Gutter widget
// ---------------------------------------------------------------------------

class LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(ScriptEdit* editor)
        : QWidget{editor}
        , m_editor{editor}
    { }

    [[nodiscard]] QSize sizeHint() const override
    {
        return QSize{m_editor->lineNumberAreaWidth(), 0};
    }

protected:
    void paintEvent(QPaintEvent* event) override { m_editor->paintLineNumbers(event); }

private:
    ScriptEdit* m_editor;
};

// ---------------------------------------------------------------------------
// ScriptEdit
// ---------------------------------------------------------------------------

ScriptEdit::ScriptEdit(QWidget* parent)
    : QPlainTextEdit{parent}
{
    setProperty("torquebusRole", QStringLiteral("code"));

    // Code wraps only where the author wrapped it.
    setLineWrapMode(QPlainTextEdit::NoWrap);

    m_lineNumbers = new LineNumberArea(this);

    applyTheme();

    connect(this, &QPlainTextEdit::blockCountChanged, this, &ScriptEdit::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &ScriptEdit::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &ScriptEdit::highlightLines);

    updateLineNumberAreaWidth();
    highlightLines();
}

void ScriptEdit::applyTheme()
{
    const QFont mono = ThemeManager::monospaceFont(m_pointSize);
    setFont(mono);
    if (document() != nullptr) {
        document()->setDefaultFont(mono);
    }
    if (m_lineNumbers != nullptr) {
        m_lineNumbers->setFont(mono);
    }

    setTabStopDistance(4 * fontMetrics().horizontalAdvance(QLatin1Char{' '}));
    updateLineNumberAreaWidth();
    highlightLines();
    if (m_lineNumbers != nullptr) {
        m_lineNumbers->update();
    }
}

int ScriptEdit::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());

    while (max >= 10) {
        max /= 10;
        ++digits;
    }

    // Three digits' worth at minimum, plus clean left & right padding
    const int charWidth = fontMetrics().horizontalAdvance(QLatin1Char{'9'});
    return 18 + charWidth * qMax(3, digits);
}

void ScriptEdit::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void ScriptEdit::updateLineNumberArea(const QRect& rect, int dy)
{
    if (dy != 0) {
        m_lineNumbers->scroll(0, dy);
    } else {
        m_lineNumbers->update(0, rect.y(), m_lineNumbers->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth();
    }
}

void ScriptEdit::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);

    const QRect contents = contentsRect();
    m_lineNumbers->setGeometry(
        QRect{contents.left(), contents.top(), lineNumberAreaWidth(), contents.height()});
}

void ScriptEdit::setErrorLine(int line)
{
    if (line == m_errorLine) {
        return;
    }

    m_errorLine = line;

    if (line > 0) {
        // Line numbers are 1-based, blocks are 0-based.
        const QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextCursor cursor{block};
            setTextCursor(cursor);
            centerCursor();
        }
    }

    highlightLines();
    m_lineNumbers->update();
}

void ScriptEdit::highlightLines()
{
    QList<QTextEdit::ExtraSelection> selections;

    if (!isReadOnly()) {
        const Theme theme = currentTheme();

        // 1. Current cursor line selection
        QTextEdit::ExtraSelection currentLineSel;
        currentLineSel.format.setBackground(theme.panelAlternate);
        currentLineSel.format.setProperty(QTextFormat::FullWidthSelection, true);
        currentLineSel.cursor = textCursor();
        currentLineSel.cursor.clearSelection();
        selections.append(currentLineSel);

        // 2. Error line selection
        if (m_errorLine > 0) {
            const QTextBlock block = document()->findBlockByNumber(m_errorLine - 1);
            if (block.isValid()) {
                QTextEdit::ExtraSelection errorSel;
                QColor errorBg = theme.error;
                errorBg.setAlpha(45);
                errorSel.format.setBackground(errorBg);
                errorSel.format.setProperty(QTextFormat::FullWidthSelection, true);
                errorSel.cursor = QTextCursor{block};
                errorSel.cursor.clearSelection();
                selections.append(errorSel);
            }
        }
    }

    setExtraSelections(selections);
}

void ScriptEdit::paintLineNumbers(QPaintEvent* event)
{
    const Theme theme = currentTheme();

    QPainter painter{m_lineNumbers};
    painter.fillRect(event->rect(), theme.panelAlternate);

    // Clean vertical dividing line separating line numbers from the code editor
    painter.setPen(QPen{theme.border, 1.0});
    painter.drawLine(m_lineNumbers->width() - 1,
                     event->rect().top(),
                     m_lineNumbers->width() - 1,
                     event->rect().bottom());

    const QFont baseFont = font();
    QFont activeFont = baseFont;
    activeFont.setWeight(QFont::DemiBold);

    QFont errorFont = baseFont;
    errorFont.setWeight(QFont::Bold);

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();

    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    const int currentLine = textCursor().blockNumber();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int blockH = static_cast<int>(blockBoundingRect(block).height());
            const QRect textRect{4, top, m_lineNumbers->width() - 12, blockH};

            // Three states: error line, current active line, and normal lines
            if (number + 1 == m_errorLine) {
                painter.setFont(errorFont);
                painter.setPen(theme.error);
            } else if (number == currentLine) {
                painter.setFont(activeFont);
                painter.setPen(theme.text);
            } else {
                painter.setFont(baseFont);
                painter.setPen(theme.textMuted);
            }

            painter.drawText(
                textRect, Qt::AlignRight | Qt::AlignVCenter, QString::number(number + 1));
        }

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++number;
    }
}

void ScriptEdit::keyPressEvent(QKeyEvent* event)
{
    // Toggle comment with Ctrl+/
    if (event->key() == Qt::Key_Slash && (event->modifiers() & Qt::ControlModifier)) {
        toggleComment();
        event->accept();
        return;
    }

    // Tab indents selection or inserts 4 spaces
    if (event->key() == Qt::Key_Tab) {
        if (textCursor().hasSelection()) {
            indentSelection();
        } else {
            insertPlainText(QStringLiteral("    "));
        }
        event->accept();
        return;
    }

    // Shift+Tab unindents selection
    if (event->key() == Qt::Key_Backtab) {
        unindentSelection();
        event->accept();
        return;
    }

    // Smart backspace: removes 4 spaces if preceded by indentation
    if (event->key() == Qt::Key_Backspace) {
        QTextCursor cursor = textCursor();
        if (!cursor.hasSelection()) {
            const int pos = cursor.positionInBlock();
            const QString blockText = cursor.block().text();
            const QString beforeCursor = blockText.left(pos);
            if (pos >= 4 && beforeCursor.trimmed().isEmpty() && (pos % 4 == 0)) {
                cursor.beginEditBlock();
                for (int i = 0; i < 4; ++i) {
                    cursor.deletePreviousChar();
                }
                cursor.endEditBlock();
                setTextCursor(cursor);
                event->accept();
                return;
            }
        }
    }

    // Smart auto-indent on Enter
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        QTextCursor cursor = textCursor();
        const QString lineText = cursor.block().text();
        const int col = cursor.positionInBlock();
        const QString textBeforeCursor = lineText.left(col);

        int leadingSpaces = 0;
        while (leadingSpaces < textBeforeCursor.size()
               && textBeforeCursor.at(leadingSpaces).isSpace()) {
            ++leadingSpaces;
        }
        QString indent = textBeforeCursor.left(leadingSpaces);

        const QString trimmed = textBeforeCursor.trimmed();
        static const QRegularExpression funcPattern{QStringLiteral("function\\s*\\(")};
        const bool opensBlock =
            trimmed.endsWith(QLatin1String("then")) || trimmed.endsWith(QLatin1String("do"))
            || trimmed.endsWith(QLatin1String("repeat")) || trimmed.endsWith(QLatin1String("else"))
            || trimmed.startsWith(QLatin1String("function")) || trimmed.contains(funcPattern)
            || trimmed.endsWith(QLatin1Char('{')) || trimmed.endsWith(QLatin1Char('('));
        if (opensBlock) {
            indent += QStringLiteral("    ");
        }

        cursor.beginEditBlock();
        if (cursor.hasSelection()) {
            cursor.removeSelectedText();
        }
        cursor.insertText(QLatin1Char('\n') + indent);
        cursor.endEditBlock();
        setTextCursor(cursor);
        ensureCursorVisible();
        event->accept();
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

void ScriptEdit::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y();
        if (delta > 0) {
            zoomIn(1.0);
        } else if (delta < 0) {
            zoomOut(1.0);
        }
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}

void ScriptEdit::toggleComment()
{
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();

    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();

    QTextCursor startCursor{document()};
    startCursor.setPosition(start);
    const int startBlock = startCursor.blockNumber();

    QTextCursor endCursor{document()};
    endCursor.setPosition(end);
    int endBlock = endCursor.blockNumber();

    if (end > start && endCursor.positionInBlock() == 0) {
        --endBlock;
    }

    // Check if all selected non-empty lines currently start with '--'
    bool allCommented = true;
    for (int b = startBlock; b <= endBlock; ++b) {
        const QTextBlock block = document()->findBlockByNumber(b);
        const QString text = block.text().trimmed();
        if (!text.isEmpty() && !text.startsWith(QLatin1String("--"))) {
            allCommented = false;
            break;
        }
    }

    for (int b = startBlock; b <= endBlock; ++b) {
        QTextBlock block = document()->findBlockByNumber(b);
        const QString text = block.text();
        QTextCursor lineCursor{block};

        if (allCommented) {
            // Remove '-- ' or '--'
            const int commentIdx = text.indexOf(QLatin1String("--"));
            if (commentIdx >= 0) {
                lineCursor.setPosition(block.position() + commentIdx);
                const int removeLen =
                    (commentIdx + 2 < text.size() && text.at(commentIdx + 2) == QLatin1Char(' '))
                        ? 3
                        : 2;
                lineCursor.setPosition(block.position() + commentIdx + removeLen,
                                       QTextCursor::KeepAnchor);
                lineCursor.removeSelectedText();
            }
        } else {
            // Add comment
            if (!text.trimmed().isEmpty()) {
                lineCursor.setPosition(block.position());
                lineCursor.insertText(QStringLiteral("-- "));
            }
        }
    }

    cursor.endEditBlock();
}

void ScriptEdit::indentSelection()
{
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();

    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();

    QTextCursor startCursor{document()};
    startCursor.setPosition(start);
    const int startBlock = startCursor.blockNumber();

    QTextCursor endCursor{document()};
    endCursor.setPosition(end);
    int endBlock = endCursor.blockNumber();

    if (end > start && endCursor.positionInBlock() == 0) {
        --endBlock;
    }

    for (int b = startBlock; b <= endBlock; ++b) {
        QTextBlock block = document()->findBlockByNumber(b);
        QTextCursor lineCursor{block};
        lineCursor.setPosition(block.position());
        lineCursor.insertText(QStringLiteral("    "));
    }

    cursor.endEditBlock();
}

void ScriptEdit::unindentSelection()
{
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();

    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();

    QTextCursor startCursor{document()};
    startCursor.setPosition(start);
    const int startBlock = startCursor.blockNumber();

    QTextCursor endCursor{document()};
    endCursor.setPosition(end);
    int endBlock = endCursor.blockNumber();

    if (end > start && endCursor.positionInBlock() == 0) {
        --endBlock;
    }

    for (int b = startBlock; b <= endBlock; ++b) {
        QTextBlock block = document()->findBlockByNumber(b);
        const QString text = block.text();
        int spacesToRemove = 0;
        while (spacesToRemove < 4 && spacesToRemove < text.size()
               && text.at(spacesToRemove) == QLatin1Char(' ')) {
            ++spacesToRemove;
        }
        if (spacesToRemove > 0) {
            QTextCursor lineCursor{block};
            lineCursor.setPosition(block.position());
            lineCursor.setPosition(block.position() + spacesToRemove, QTextCursor::KeepAnchor);
            lineCursor.removeSelectedText();
        }
    }

    cursor.endEditBlock();
}

void ScriptEdit::insertSnippet(const QString& snippet)
{
    if (snippet.isEmpty()) {
        return;
    }

    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();

    if (cursor.hasSelection()) {
        cursor.removeSelectedText();
    }

    // Preserve the current line's leading indentation for multi-line snippets
    const QString lineText = cursor.block().text();
    const int col = cursor.positionInBlock();
    int leadingSpaces = 0;
    while (leadingSpaces < lineText.size() && lineText.at(leadingSpaces).isSpace()
           && leadingSpaces < col) {
        ++leadingSpaces;
    }
    const QString baseIndent = lineText.left(leadingSpaces);

    if (snippet.contains(QLatin1Char('\n')) && !baseIndent.isEmpty()) {
        const QStringList lines = snippet.split(QLatin1Char('\n'));
        QStringList indented;
        indented.reserve(lines.size());
        for (int i = 0; i < lines.size(); ++i) {
            if (i > 0 && !lines[i].trimmed().isEmpty()) {
                indented.append(baseIndent + lines[i]);
            } else {
                indented.append(lines[i]);
            }
        }
        cursor.insertText(indented.join(QLatin1Char('\n')));
    } else {
        cursor.insertText(snippet);
    }

    cursor.endEditBlock();
    setTextCursor(cursor);
    ensureCursorVisible();
    setFocus();
}

void ScriptEdit::zoomIn(qreal delta)
{
    if (m_pointSize + delta <= 32.0) {
        m_pointSize += delta;
        applyTheme();
    }
}

void ScriptEdit::zoomOut(qreal delta)
{
    if (m_pointSize - delta >= 6.0) {
        m_pointSize -= delta;
        applyTheme();
    }
}

void ScriptEdit::resetZoom()
{
    m_pointSize = 10.0;
    applyTheme();
}

bool ScriptEdit::findText(const QString& query, bool matchCase, bool wholeWords, bool backward)
{
    if (query.isEmpty() || document() == nullptr) {
        return false;
    }

    QTextDocument::FindFlags flags;
    if (matchCase) {
        flags |= QTextDocument::FindCaseSensitively;
    }
    if (wholeWords) {
        flags |= QTextDocument::FindWholeWords;
    }
    if (backward) {
        flags |= QTextDocument::FindBackward;
    }

    QTextCursor found = document()->find(query, textCursor(), flags);

    if (found.isNull()) {
        // Wrap around search
        QTextCursor wrapCursor{document()};
        wrapCursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        found = document()->find(query, wrapCursor, flags);
    }

    if (!found.isNull()) {
        setTextCursor(found);
        centerCursor();
        return true;
    }

    return false;
}

bool ScriptEdit::replaceCurrent(const QString& query,
                                const QString& replacement,
                                bool matchCase,
                                bool wholeWords)
{
    if (query.isEmpty()) {
        return false;
    }

    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        const QString selected = cursor.selectedText();
        const bool matches =
            matchCase ? (selected == query) : (selected.compare(query, Qt::CaseInsensitive) == 0);
        if (matches) {
            cursor.insertText(replacement);
            setTextCursor(cursor);
            findText(query, matchCase, wholeWords, false);
            return true;
        }
    }

    return findText(query, matchCase, wholeWords, false);
}

int ScriptEdit::replaceAll(const QString& query,
                           const QString& replacement,
                           bool matchCase,
                           bool wholeWords)
{
    if (query.isEmpty() || document() == nullptr) {
        return 0;
    }

    QTextDocument::FindFlags flags;
    if (matchCase) {
        flags |= QTextDocument::FindCaseSensitively;
    }
    if (wholeWords) {
        flags |= QTextDocument::FindWholeWords;
    }

    int count = 0;
    QTextCursor cursor{document()};
    cursor.beginEditBlock();

    while (true) {
        QTextCursor found = document()->find(query, cursor, flags);
        if (found.isNull()) {
            break;
        }
        found.insertText(replacement);
        cursor = found;
        ++count;
    }

    cursor.endEditBlock();
    return count;
}

int ScriptEdit::currentLine() const
{
    return textCursor().blockNumber() + 1;
}

int ScriptEdit::currentColumn() const
{
    return textCursor().positionInBlock() + 1;
}

int ScriptEdit::lineCount() const
{
    return blockCount();
}

int ScriptEdit::characterCount() const
{
    return document() != nullptr ? qMax(0, document()->characterCount() - 1) : 0;
}

} // namespace torquebus::ui
