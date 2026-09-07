// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A plain text editor that knows about lines.
//
// Two things only, and both exist for the same reason: a Lua error says
// `ecu_motor.lua:42: '=' expected near 'end'`, and a text box with no line
// numbers turns that 42 into counting with a finger down the screen.
//
//   * a gutter with line numbers, and
//   * one line that can be marked as the one the error is on.
//
// Everything else - completion, folding, a project tree of scripts - is not
// here on purpose. This is the editor for changing a number and pressing
// Reload, and the people who want an IDE already have one open; a script kept
// in a file goes on being editable there, and this panel picks the change up.

#pragma once

#include <QPlainTextEdit>

class QPaintEvent;
class QResizeEvent;
class QWidget;

namespace torquebus::ui {

class ScriptEdit final : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit ScriptEdit(QWidget* parent = nullptr);

    /// Painted by the gutter widget, which is a child that forwards to this.
    void paintLineNumbers(QPaintEvent* event);

    [[nodiscard]] int lineNumberAreaWidth() const;

    /// Marks one line as the one an error is on, and scrolls to it. Zero -
    /// which is not a line number - clears the mark.
    ///
    /// Marked rather than jumped to and selected: the error is information
    /// about the text, and taking the cursor away from where somebody is typing
    /// to make a point about a line they can already see is how an editor
    /// becomes annoying to use.
    void setErrorLine(int line);

    [[nodiscard]] int errorLine() const noexcept { return m_errorLine; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightLines();

private:
    QWidget* m_lineNumbers{nullptr};

    /// 1-based, or 0 for "no error".
    int m_errorLine{0};
};

} // namespace torquebus::ui
