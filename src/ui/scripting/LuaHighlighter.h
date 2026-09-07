// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Colour for Lua, and for the words this tool adds to it.
//
// Deliberately small: keywords, numbers, strings, comments, and the TorqueBus
// bindings. That last group is the one worth the file - `emit`, `cyclic`,
// `uds_did` and the rest are what makes a script an ECU rather than a program,
// and seeing them coloured differently from the language is what tells somebody
// reading an example which half they can look up in the Lua manual and which
// half is documented in docs/scripting.md.
//
// Colours come from the theme rather than from constants here, so a script
// reads correctly in Light as well as Dark - and so an accent change does not
// leave one panel painted in the old one.

#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

class QTextDocument;

namespace torquebus::ui {

class LuaHighlighter final : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit LuaHighlighter(QTextDocument* document);

    /// Rebuilds the formats from the current theme and repaints.
    void applyTheme();

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule final {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    QVector<Rule> m_rules;

    /// Long comments (`--[[ ... ]]`) span blocks, so they are matched with the
    /// block state rather than by a rule.
    QRegularExpression m_longCommentStart;
    QRegularExpression m_longCommentEnd;
    QTextCharFormat m_commentFormat;
};

} // namespace torquebus::ui
