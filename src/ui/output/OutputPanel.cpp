// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/output/OutputPanel.h"

#include "ui/theme/ThemeManager.h"

#include <QDateTime>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

namespace torquebus::ui {
namespace {

QString levelTag(OutputPanel::Level level)
{
    switch (level) {
    case OutputPanel::Level::Debug:   return QStringLiteral("DEBUG");
    case OutputPanel::Level::Info:    return QStringLiteral("INFO ");
    case OutputPanel::Level::Warning: return QStringLiteral("WARN ");
    case OutputPanel::Level::Error:   return QStringLiteral("ERROR");
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
    case OutputPanel::Level::Debug:   return theme.textMuted;
    case OutputPanel::Level::Info:    return theme.text;
    case OutputPanel::Level::Warning: return theme.warning;
    case OutputPanel::Level::Error:   return theme.error;
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

    cursor.insertText(QStringLiteral("%1  %2  %3")
                          .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                               levelTag(level),
                               message),
                      format);

    if (wasAtBottom) {
        scrollBar->setValue(scrollBar->maximum());
    }
}

void OutputPanel::appendInfo(const QString& message)    { append(Level::Info, message); }
void OutputPanel::appendWarning(const QString& message) { append(Level::Warning, message); }
void OutputPanel::appendError(const QString& message)   { append(Level::Error, message); }

void OutputPanel::clear()
{
    m_view->clear();
}

} // namespace torquebus::ui
