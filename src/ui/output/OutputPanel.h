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

private:
    QPlainTextEdit* m_view{nullptr};
};

} // namespace torquebus::ui
