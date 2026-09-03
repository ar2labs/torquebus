// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The DBC Explorer: what a database says, in the shape the database says it.
//
// Three levels - file, message, signal - because that is how a .dbc is
// organised and how an engineer asks about it ("what is on 0x101?", then "what
// is in that message?"). Flattening it into a signal list would make the
// second question unanswerable.
//
// This panel browses databases. It does not feed them to the pipeline: a
// decoder block names its own .dbc by path, so that a project file describes
// its own decoding without depending on what happens to be open in a panel.
// Loading a file here and wiring a decoder there are deliberately separate -
// the alternative is a graph whose behaviour depends on UI state.

#pragma once

#include "core/database/CanMessage.h"

#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus::ui {

class DatabasePanel final : public QWidget {
    Q_OBJECT

public:
    explicit DatabasePanel(QWidget* parent = nullptr);

    /// Loads a .dbc and adds it to the tree.
    ///
    /// Returns false and leaves the tree untouched on a parse failure; the
    /// reason goes out through databaseFailed() so the Output panel can carry
    /// it. A modal box here would be wrong - the message names a line number,
    /// and a line number is something you want to keep looking at.
    bool loadDatabase(const QString& path);

    /// Forgets every loaded database.
    void clear();

    [[nodiscard]] int databaseCount() const;

    /// The loaded databases, for anything that decodes with them.
    ///
    /// Shared, not borrowed: the trace keeps its copy of the pointers, so
    /// closing this panel mid-measurement leaves every definition a displayed
    /// row points at alive.
    [[nodiscard]] std::vector<std::shared_ptr<const CanDatabase>> databases() const;

Q_SIGNALS:
    void databaseLoaded(const QString& path, int messageCount, int signalCount);
    void databaseFailed(const QString& path, const QString& reason);

    /// A signal was picked. Carries enough to identify it without exposing a
    /// pointer into a database the receiver does not own.
    void signalSelected(const QString& databasePath,
                        const QString& messageName,
                        const QString& signalName);

private Q_SLOTS:
    void onFilterChanged(const QString& text);
    void onCurrentItemChanged(QTreeWidgetItem* current);
    void onThemeChanged();

private:
    void buildUi();
    void addDatabaseToTree(const CanDatabase& database);
    void applyIcons();
    void updateSummary();

    /// Hides the rows that do not match, keeping a message visible when one of
    /// its signals does. Filtering by hiding rather than by rebuilding, so the
    /// tree's expansion state survives typing.
    [[nodiscard]] bool applyFilter(QTreeWidgetItem* item, const QString& needle);

    QTreeWidget* m_tree{nullptr};
    QLineEdit* m_filter{nullptr};
    QLabel* m_summary{nullptr};

    /// Held by shared_ptr for the same reason the decoder node does: a
    /// DecodedSignal points at these definitions, and anything this panel hands
    /// out has to stay valid.
    std::vector<std::shared_ptr<CanDatabase>> m_databases;
};

} // namespace torquebus::ui
