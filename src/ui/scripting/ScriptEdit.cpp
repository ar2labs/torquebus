// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/scripting/ScriptEdit.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QFontDatabase>
#include <QPainter>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextEdit>

namespace torquebus::ui {
namespace {

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// The gutter. A child widget rather than a margin painted by the editor,
/// because QPlainTextEdit scrolls its viewport and a margin would scroll with
/// it - the standard Qt arrangement, for the standard reason.
class LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(ScriptEdit* editor)
        : QWidget{editor}
        , m_editor{editor}
    {
    }

    [[nodiscard]] QSize sizeHint() const override
    {
        return QSize{m_editor->lineNumberAreaWidth(), 0};
    }

protected:
    void paintEvent(QPaintEvent* event) override { m_editor->paintLineNumbers(event); }

private:
    ScriptEdit* m_editor{nullptr};
};

} // namespace

ScriptEdit::ScriptEdit(QWidget* parent)
    : QPlainTextEdit{parent}
{
    // The fixed-width font the platform actually has, asked for by role rather
    // than by name: "Consolas" is a guess that is wrong on two of the three
    // platforms this is meant to build on.
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    // A script is code: it wraps where the author wrapped it, and nowhere else.
    // Soft-wrapped Lua puts a continuation where no line ends, which is exactly
    // the confusion an error line number is supposed to remove.
    setLineWrapMode(QPlainTextEdit::NoWrap);

    setTabStopDistance(4 * fontMetrics().horizontalAdvance(QLatin1Char{' '}));

    m_lineNumbers = new LineNumberArea(this);

    connect(this, &QPlainTextEdit::blockCountChanged, this,
            &ScriptEdit::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &ScriptEdit::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &ScriptEdit::highlightLines);

    updateLineNumberAreaWidth();
    highlightLines();
}

int ScriptEdit::lineNumberAreaWidth() const
{
    int digits = 1;
    int lines = qMax(1, blockCount());

    while (lines >= 10) {
        lines /= 10;
        ++digits;
    }

    // Three digits' worth at minimum, so the text does not shift sideways the
    // moment a script grows past line 99.
    return 10 + fontMetrics().horizontalAdvance(QLatin1Char{'9'}) * qMax(3, digits);
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

    const QRect area = contentsRect();
    m_lineNumbers->setGeometry(
        QRect{area.left(), area.top(), lineNumberAreaWidth(), area.height()});
}

void ScriptEdit::setErrorLine(int line)
{
    m_errorLine = line;

    if (line > 0) {
        // Scrolled into view without moving the cursor: the point is to show
        // the line, not to take over where somebody was typing.
        const QTextBlock block = document()->findBlockByNumber(line - 1);

        if (block.isValid()) {
            QTextCursor cursor{block};
            const QTextCursor kept = textCursor();

            setTextCursor(cursor);
            ensureCursorVisible();
            setTextCursor(kept);
        }
    }

    highlightLines();
    m_lineNumbers->update();
}

void ScriptEdit::highlightLines()
{
    const Theme theme = currentTheme();

    QList<QTextEdit::ExtraSelection> selections;

    // The error first, so that when the cursor is on the same line the error
    // colour is the one drawn on top - which is the one worth seeing.
    if (m_errorLine > 0) {
        const QTextBlock block = document()->findBlockByNumber(m_errorLine - 1);

        if (block.isValid()) {
            QColor tint = theme.error;
            tint.setAlpha(48);

            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(tint);
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = QTextCursor{block};
            selection.cursor.clearSelection();

            selections.append(selection);
        }
    }

    if (!isReadOnly()) {
        QColor tint = theme.hover;
        tint.setAlpha(40);

        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(tint);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();

        selections.append(selection);
    }

    setExtraSelections(selections);
}

void ScriptEdit::paintLineNumbers(QPaintEvent* event)
{
    const Theme theme = currentTheme();

    QPainter painter{m_lineNumbers};
    painter.fillRect(event->rect(), theme.panelAlternate);

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();

    int top = static_cast<int>(
        blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    const int currentLine = textCursor().blockNumber();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            // Three states, and each one earns its colour: the line an error is
            // on, the line the cursor is on, and every other line.
            if (number + 1 == m_errorLine) {
                painter.setPen(theme.error);
            } else if (number == currentLine) {
                painter.setPen(theme.text);
            } else {
                painter.setPen(theme.textMuted);
            }

            painter.drawText(0, top, m_lineNumbers->width() - 5,
                             fontMetrics().height(), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(number + 1));
        }

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++number;
    }
}

} // namespace torquebus::ui
