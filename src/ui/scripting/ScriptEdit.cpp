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
    { }

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
    setProperty("torquebusRole", QStringLiteral("code"));

    // A script is code: it wraps where the author wrapped it, and nowhere else.
    // Soft-wrapped Lua puts a continuation where no line ends, which is exactly
    // the confusion an error line number is supposed to remove.
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
    const QFont mono = ThemeManager::monospaceFont(10.0);
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
    int lines = qMax(1, blockCount());

    while (lines >= 10) {
        lines /= 10;
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

} // namespace torquebus::ui
