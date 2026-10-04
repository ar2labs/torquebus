// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/scripting/LuaHighlighter.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QStringList>

namespace torquebus::ui {
namespace {

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// Lua 5.5, exactly. Not "roughly Lua": a keyword list that includes something
/// which is not one teaches a wrong thing to whoever is learning from the
/// colours.
const QStringList& luaKeywords()
{
    static const QStringList keywords{
        QStringLiteral("and"),   QStringLiteral("break"),  QStringLiteral("do"),
        QStringLiteral("else"),  QStringLiteral("elseif"), QStringLiteral("end"),
        QStringLiteral("false"), QStringLiteral("for"),    QStringLiteral("function"),
        QStringLiteral("goto"),  QStringLiteral("if"),     QStringLiteral("in"),
        QStringLiteral("local"), QStringLiteral("nil"),    QStringLiteral("not"),
        QStringLiteral("or"),    QStringLiteral("repeat"), QStringLiteral("return"),
        QStringLiteral("then"),  QStringLiteral("true"),   QStringLiteral("until"),
        QStringLiteral("while"),
    };

    return keywords;
}

/// The bindings and callbacks this tool adds. Kept in one place next to the
/// list in LuaEcuNode.h - when a binding is added there and not here, the only
/// symptom is that it is not coloured, which is the cheapest possible way for
/// these two lists to disagree.
const QStringList& torqueBusNames()
{
    static const QStringList names{
        // Callbacks the node looks for.
        QStringLiteral("on_enable"),
        QStringLiteral("on_disable"),
        QStringLiteral("on_timer"),
        QStringLiteral("on_message"),
        QStringLiteral("on_uds_request"),
        QStringLiteral("on_security_seed"),
        // Bindings a script calls back.
        QStringLiteral("emit"),
        QStringLiteral("emit_signal"),
        QStringLiteral("decode"),
        QStringLiteral("every"),
        QStringLiteral("cyclic"),
        QStringLiteral("stop_cyclic"),
        QStringLiteral("fault"),
        QStringLiteral("bus_last"),
        QStringLiteral("bus_stats"),
        QStringLiteral("set_timer"),
        QStringLiteral("log_message"),
        QStringLiteral("get_time_us"),
        QStringLiteral("uds_did"),
        QStringLiteral("uds_dtc"),
        QStringLiteral("uds_clear_dtc"),
        QStringLiteral("uds_session"),
        // Globals the node sets.
        QStringLiteral("parameters"),
        QStringLiteral("node_name"),
        QStringLiteral("channel"),
    };

    return names;
}

} // namespace

LuaHighlighter::LuaHighlighter(QTextDocument* document)
    : QSyntaxHighlighter{document}
{
    m_longCommentStart = QRegularExpression{QStringLiteral("--\\[\\[")};
    m_longCommentEnd = QRegularExpression{QStringLiteral("\\]\\]")};

    applyTheme();
}

void LuaHighlighter::applyTheme()
{
    const Theme theme = currentTheme();

    m_rules.clear();

    const auto add = [this](const QString& pattern, const QTextCharFormat& format) {
        m_rules.append(Rule{QRegularExpression{pattern}, format});
    };

    // --- Keywords ---------------------------------------------------------
    QTextCharFormat keyword;
    keyword.setForeground(theme.accent);
    keyword.setFontWeight(QFont::DemiBold);

    for (const QString& word : luaKeywords()) {
        add(QStringLiteral("\\b%1\\b").arg(word), keyword);
    }

    // --- What this tool adds ----------------------------------------------
    QTextCharFormat binding;
    binding.setForeground(theme.tx);

    for (const QString& name : torqueBusNames()) {
        add(QStringLiteral("\\b%1\\b").arg(name), binding);
    }

    // --- Numbers, including hex, which CAN scripts are full of ------------
    QTextCharFormat number;
    number.setForeground(theme.rx);

    add(QStringLiteral("\\b0[xX][0-9a-fA-F]+\\b"), number);
    add(QStringLiteral("\\b\\d+(\\.\\d+)?([eE][-+]?\\d+)?\\b"), number);

    // --- Strings ----------------------------------------------------------
    //
    // Escapes included in the match, so that "\xAA\xBB" - which is how a payload
    // is written - is one coloured string rather than three fragments.
    QTextCharFormat text;
    text.setForeground(theme.success);

    add(QStringLiteral("\"(\\\\.|[^\"\\\\])*\""), text);
    add(QStringLiteral("'(\\\\.|[^'\\\\])*'"), text);

    // --- Comments, last, so a `--` inside a string does not win ------------
    m_commentFormat = QTextCharFormat{};
    m_commentFormat.setForeground(theme.textMuted);

    add(QStringLiteral("--[^\\[\\n][^\\n]*"), m_commentFormat);
    add(QStringLiteral("--$"), m_commentFormat);

    rehighlight();
}

void LuaHighlighter::highlightBlock(const QString& text)
{
    for (const Rule& rule : m_rules) {
        QRegularExpressionMatchIterator matches = rule.pattern.globalMatch(text);

        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            setFormat(static_cast<int>(match.capturedStart()),
                      static_cast<int>(match.capturedLength()),
                      rule.format);
        }
    }

    // --- Long comments, which span blocks ---------------------------------
    setCurrentBlockState(0);

    int start = 0;

    if (previousBlockState() != 1) {
        start = static_cast<int>(text.indexOf(m_longCommentStart));
    }

    while (start >= 0) {
        const QRegularExpressionMatch end = m_longCommentEnd.match(text, start);

        int length = 0;

        if (end.hasMatch()) {
            length = static_cast<int>(end.capturedEnd()) - start;
        } else {
            setCurrentBlockState(1);
            length = static_cast<int>(text.length()) - start;
        }

        setFormat(start, length, m_commentFormat);

        if (!end.hasMatch()) {
            break;
        }

        // Widened before the addition, not after. Casting the sum says the
        // width was considered while still doing the arithmetic in int - which
        // is the one place it could go wrong.
        start = static_cast<int>(text.indexOf(
            m_longCommentStart, static_cast<qsizetype>(start) + static_cast<qsizetype>(length)));
    }
}

} // namespace torquebus::ui
