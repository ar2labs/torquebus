// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The application log, as the user sees it.
//
// This is a sink, not a logger: core code emits log records through the
// logging subsystem and never reaches into a widget (rule #7). The panel
// installs itself as one sink among others - the file logger being the sink
// that keeps working when this panel is closed.

#pragma once

#include <QString>
#include <QWidget>

class QPlainTextEdit;

namespace torquebus::ui {

class OutputPanel final : public QWidget {
    Q_OBJECT

public:
    enum class Level {
        Debug,
        Info,
        Warning,
        Error
    };
    Q_ENUM(Level)

    explicit OutputPanel(QWidget* parent = nullptr);

    /// Maximum number of lines kept in the view. Older lines are discarded;
    /// the file log keeps everything.
    void setMaximumLines(int lines);

public Q_SLOTS:
    void append(torquebus::ui::OutputPanel::Level level, const QString& message);

    void appendInfo(const QString& message);
    void appendWarning(const QString& message);
    void appendError(const QString& message);

    void clear();

private Q_SLOTS:
    /// Recolours every line already in the view.
    ///
    /// The level's colour is baked into a QTextCharFormat when the line is
    /// inserted, and a theme change does not revisit it - so after a switch the
    /// whole scrollback was still painted in the colours of the theme it was
    /// written under. On a dark window that is dark text on a dark background,
    /// which is what the log looked like in every screenshot taken after
    /// toggling the theme.
    ///
    /// Each block carries its level in QTextBlock::userState, so nothing has to
    /// be stored twice and the text itself is never rebuilt.
    void onThemeChanged();

private:
    QPlainTextEdit* m_view{nullptr};
};

} // namespace torquebus::ui
