// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A plain text editor designed for writing and debugging Lua scripts.
// Provides line numbers, error highlighting, auto-indentation, smart tabs,
// comment toggling, zoom controls, and find/replace support.

#pragma once

#include <QPlainTextEdit>

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
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
    void setErrorLine(int line);

    [[nodiscard]] int errorLine() const noexcept { return m_errorLine; }

    /// Re-applies font and colors matching the CAN Trace monospace standard.
    void applyTheme();

    /// Professional editing actions
    void toggleComment();
    void indentSelection();
    void unindentSelection();
    void insertSnippet(const QString& snippet);

    /// Zoom controls
    void zoomIn(qreal delta = 1.0);
    void zoomOut(qreal delta = 1.0);
    void resetZoom();
    [[nodiscard]] qreal pointSize() const noexcept { return m_pointSize; }

    /// Search and replace operations
    bool findText(const QString& query, bool matchCase, bool wholeWords, bool backward);
    bool replaceCurrent(const QString& query,
                        const QString& replacement,
                        bool matchCase,
                        bool wholeWords);
    int
    replaceAll(const QString& query, const QString& replacement, bool matchCase, bool wholeWords);

    /// Cursor & document position info
    [[nodiscard]] int currentLine() const;
    [[nodiscard]] int currentColumn() const;
    [[nodiscard]] int lineCount() const;
    [[nodiscard]] int characterCount() const;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private Q_SLOTS:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightLines();

private:
    QWidget* m_lineNumbers{nullptr};

    /// 1-based, or 0 for "no error".
    int m_errorLine{0};

    /// Base font point size (default 10.0pt).
    qreal m_pointSize{10.0};
};

} // namespace torquebus::ui
