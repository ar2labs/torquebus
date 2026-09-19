// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/output/OutputPanel.h"

#include "ui/theme/ThemeManager.h"

#include <QDateTime>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

namespace torquebus::ui {
namespace {

QString levelTag(OutputPanel::Level level)
{
    switch (level) {
    case OutputPanel::Level::Debug:
        return QStringLiteral("DEBUG");
    case OutputPanel::Level::Info:
        return QStringLiteral("INFO ");
    case OutputPanel::Level::Warning:
        return QStringLiteral("WARN ");
    case OutputPanel::Level::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("     ");
}

QColor levelColor(OutputPanel::Level level)
{
    const ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr) {
        return {};
    }

    const Theme& theme = themes->theme();
    switch (level) {
    case OutputPanel::Level::Debug:
        return theme.textMuted;
    case OutputPanel::Level::Info:
        return theme.text;
    case OutputPanel::Level::Warning:
        return theme.warning;
    case OutputPanel::Level::Error:
        return theme.error;
    }
    return theme.text;
}

} // namespace

OutputPanel::OutputPanel(QWidget* parent)
    : QWidget{parent}
{
    m_view = new QPlainTextEdit(this);
    m_view->setObjectName(QStringLiteral("torquebusConsole"));
    m_view->setReadOnly(true);
    m_view->setUndoRedoEnabled(false);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setMaximumBlockCount(5000);
    m_view->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }
}

void OutputPanel::setMaximumLines(int lines)
{
    m_view->setMaximumBlockCount(lines);
}

void OutputPanel::append(Level level, const QString& message)
{
    // "Was the view already at the bottom?" is asked before inserting, so that
    // a user reading scrollback is not yanked to the end by a background event.
    QScrollBar* scrollBar = m_view->verticalScrollBar();
    const bool wasAtBottom = scrollBar->value() >= scrollBar->maximum() - 4;

    QTextCharFormat format;
    format.setForeground(levelColor(level));

    QTextCursor cursor = m_view->textCursor();
    cursor.movePosition(QTextCursor::End);

    if (!m_view->document()->isEmpty()) {
        cursor.insertBlock();
    }

    cursor.insertText(
        QStringLiteral("%1  %2  %3")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                 levelTag(level),
                 message),
        format);

    // The level, kept with the line so a theme change can recolour it without
    // a parallel list of entries to fall out of step with the document.
    cursor.block().setUserState(static_cast<int>(level));

    if (wasAtBottom) {
        scrollBar->setValue(scrollBar->maximum());
    }
}

void OutputPanel::onThemeChanged()
{
    QTextDocument* document = m_view->document();

    // One undo-less edit rather than one per line: without this the view
    // repaints and rescrolls for every block it touches.
    QTextCursor cursor{document};
    cursor.beginEditBlock();

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const int state = block.userState();
        if (state < 0) {
            // A block written before this was tracked, or one Qt created on its
            // own. Leaving it alone is better than guessing a level for it.
            continue;
        }

        QTextCharFormat format;
        format.setForeground(levelColor(static_cast<OutputPanel::Level>(state)));

        // Start to end explicitly, rather than BlockUnderCursor: that selection
        // reaches back over the preceding paragraph separator, so every line
        // would also restyle a character belonging to the line above it.
        QTextCursor line{block};
        line.movePosition(QTextCursor::StartOfBlock);
        line.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        line.mergeCharFormat(format);
    }

    cursor.endEditBlock();
}

void OutputPanel::appendInfo(const QString& message)
{
    append(Level::Info, message);
}
void OutputPanel::appendWarning(const QString& message)
{
    append(Level::Warning, message);
}
void OutputPanel::appendError(const QString& message)
{
    append(Level::Error, message);
}

void OutputPanel::clear()
{
    m_view->clear();
}

} // namespace torquebus::ui
