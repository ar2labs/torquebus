// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Choosing signals from the databases that are loaded, instead of typing their names.
//
// A signal name typed into a form is a signal name typed wrongly some of the time, and the form
// says nothing until a gauge shows dashes. This is the other way to name one: a list of every
// signal of every loaded database that narrows as the first letters are typed, shows beside it what
// the database knows (the unit, the range, how often it is sent, the names of its values), and
// says which widget it would make.
//
// It is used two ways, and the difference is only what it lets be selected:
//
//   * **Add widgets** - from the Dashboard's menu. Several signals can be chosen, and a message:
//     each signal becomes the widget it suits (or the kind picked in the box), and a message
//     becomes a table of all its signals.
//   * **Choose a signal** - from a widget's form. One signal, to bind the widget to.
//
// The search is the dictionary's (SignalDictionary::search), so what ranks first here ranks first
// everywhere. With nothing typed the list is grouped by message, as the DBC Explorer's is; with
// something typed it is flat, best match first, because a ranked list broken into folders is not
// ranked.

#pragma once

#include "core/dashboard/DashboardDescription.h"
#include "core/dashboard/SignalDictionary.h"

#include <QDialog>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus::ui {

class SignalPickerDialog final : public QDialog {
    Q_OBJECT

public:
    enum class Mode : std::uint8_t {
        /// Several signals, or messages, and the kind of widget to make of them.
        AddWidgets,

        /// One signal, for a widget that is already there.
        ChooseSignal,

        /// One message, for a table that is already there.
        ChooseMessage,
    };

    /// `dictionary` is borrowed and must outlive the dialog, which is modal and short-lived.
    SignalPickerDialog(const SignalDictionary& dictionary, Mode mode, QWidget* parent = nullptr);

    /// The signals selected, in the databases' order. Copies, so that they outlive the dialog.
    [[nodiscard]] std::vector<DictionaryEntry> chosenSignals() const;

    /// The messages selected, for tables.
    [[nodiscard]] std::vector<std::string> chosenMessages() const;

    /// The kind picked, or nothing for "automatic": let each signal choose its own.
    [[nodiscard]] std::optional<DashboardWidgetKind> chosenKind() const;

    // --- For a test, and for anything that would drive the dialog without a mouse --------------

    void setSearchText(const QString& text);

    /// How many signals the list shows now.
    [[nodiscard]] int shownSignalCount() const;

    /// Adds the signal - or the message, when `signal` is empty - to the selection and makes it the
    /// current item, as a Ctrl-click does. False when the list does not show it.
    bool select(std::string_view message, std::string_view signal = {});

    /// What the details pane says now.
    [[nodiscard]] QString detailsText() const;

private Q_SLOTS:
    void onSearchChanged();
    void onSelectionChanged();
    void onItemDoubleClicked(QTreeWidgetItem* item);

private:
    void buildUi();
    void populate();

    /// The entry an item stands for, or nullptr for a message or nothing.
    [[nodiscard]] const DictionaryEntry* entryOf(const QTreeWidgetItem* item) const;

    const SignalDictionary& m_dictionary;
    Mode m_mode;

    QLineEdit* m_search{nullptr};
    QTreeWidget* m_tree{nullptr};
    QLabel* m_details{nullptr};
    QLabel* m_hint{nullptr};
    QComboBox* m_kind{nullptr};
    QPushButton* m_ok{nullptr};
};

} // namespace torquebus::ui
