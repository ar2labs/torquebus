// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/SignalPickerDialog.h"

#include "ui/dashboard/DashboardText.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace torquebus::ui {
namespace {

enum Column : int {
    ColumnName = 0,
    ColumnUnit,
    ColumnRange,
    ColumnEvery,
    ColumnWidget,
    ColumnCount,
};

/// An item's role for the index of its entry in the dictionary, or the name of its message.
constexpr int kEntryRole = Qt::UserRole;
constexpr int kMessageRole = Qt::UserRole + 1;

/// More than a person scrolls; the search is ranked, and what is not in the first screens is not
/// what they were looking for.
constexpr std::size_t kMostResults = 300;

[[nodiscard]] QString plain(double value)
{
    return QString::number(value, 'g', 10);
}

} // namespace

SignalPickerDialog::SignalPickerDialog(const SignalDictionary& dictionary,
                                       Mode mode,
                                       QWidget* parent)
    : QDialog{parent}
    , m_dictionary{dictionary}
    , m_mode{mode}
{
    buildUi();
    populate();
    onSelectionChanged();
}

void SignalPickerDialog::buildUi()
{
    switch (m_mode) {
    case Mode::AddWidgets:
        setWindowTitle(tr("Add from Database"));
        break;
    case Mode::ChooseSignal:
        setWindowTitle(tr("Choose a Signal"));
        break;
    case Mode::ChooseMessage:
        setWindowTitle(tr("Choose a Message"));
        break;
    }

    setModal(true);
    resize(900, 560);

    QString hint;

    switch (m_mode) {
    case Mode::AddWidgets:
        hint = tr("Select signals to add a widget for each, or a message to add a table of all its "
                  "signals. Type to narrow the list: part of a name, its initials, a unit.");
        break;
    case Mode::ChooseSignal:
        hint = tr("Select the signal this widget should read. Type to narrow the list: part of a "
                  "name, its initials, a unit.");
        break;
    case Mode::ChooseMessage:
        hint = tr("Select the message this table should list. Type to narrow the list.");
        break;
    }

    m_hint = new QLabel(hint);
    m_hint->setWordWrap(true);
    m_hint->setProperty("torquebusRole", QStringLiteral("caption"));

    m_search = new QLineEdit;
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText(
        tr("Search %n signal(s)", nullptr, static_cast<int>(m_dictionary.size())));

    m_tree = new QTreeWidget;
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("Signal"), tr("Unit"), tr("Range"), tr("Every"), tr("Widget")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setSelectionMode(m_mode == Mode::AddWidgets ? QAbstractItemView::ExtendedSelection
                                                        : QAbstractItemView::SingleSelection);

    // Asked for a message, the list is of messages: their signals are not what is being chosen.
    m_tree->setRootIsDecorated(m_mode != Mode::ChooseMessage);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(ColumnName, QHeaderView::Stretch);
    m_tree->setColumnWidth(ColumnUnit, 70);
    m_tree->setColumnWidth(ColumnRange, 150);
    m_tree->setColumnWidth(ColumnEvery, 70);
    m_tree->setColumnWidth(ColumnWidget, 80);

    m_details = new QLabel;
    m_details->setWordWrap(true);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setMargin(8);
    m_details->setMinimumWidth(240);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(m_tree);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ok = buttons->button(QDialogButtonBox::Ok);

    auto* bottom = new QHBoxLayout;

    if (m_mode == Mode::AddWidgets) {
        m_kind = new QComboBox;
        m_kind->addItem(tr("Automatic - the widget each signal suits"), -1);

        for (const DashboardWidgetKind kind : readerKinds()) {
            m_kind->addItem(displayNameOf(kind), static_cast<int>(kind));
        }

        m_kind->setToolTip(tr("Make every selected signal this kind of widget instead of the one "
                              "it suits. Messages always make tables."));

        bottom->addWidget(new QLabel(tr("Show as")));
        bottom->addWidget(m_kind, 1);
    } else {
        bottom->addStretch(1);
    }

    bottom->addWidget(buttons);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_hint);
    layout->addWidget(m_search);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottom);

    connect(m_search, &QLineEdit::textChanged, this, &SignalPickerDialog::onSearchChanged);
    connect(
        m_tree, &QTreeWidget::itemSelectionChanged, this, &SignalPickerDialog::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        onItemDoubleClicked(item);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_search->setFocus();
}

const DictionaryEntry* SignalPickerDialog::entryOf(const QTreeWidgetItem* item) const
{
    if (item == nullptr) {
        return nullptr;
    }

    const QVariant index = item->data(ColumnName, kEntryRole);

    if (!index.isValid()) {
        return nullptr;
    }

    const auto position = static_cast<std::size_t>(index.toLongLong());

    return position < m_dictionary.entries().size() ? &m_dictionary.entries()[position] : nullptr;
}

void SignalPickerDialog::populate()
{
    m_tree->clear();

    const auto addSignal =
        [this](QTreeWidgetItem* parent, const DictionaryEntry& entry, bool qualified) {
            auto* item = new QTreeWidgetItem(parent);

            const auto index = static_cast<qlonglong>(&entry - m_dictionary.entries().data());
            item->setData(ColumnName, kEntryRole, index);

            item->setText(ColumnName,
                          QString::fromStdString(qualified ? entry.qualified() : entry.signal));
            item->setText(ColumnUnit, QString::fromStdString(entry.unit));
            item->setText(ColumnRange,
                          plain(entry.minimum) + QStringLiteral(" … ") + plain(entry.maximum));

            if (entry.cycleTimeMs > 0) {
                item->setText(ColumnEvery, tr("%1 ms").arg(entry.cycleTimeMs));
            }

            item->setText(ColumnWidget, displayNameOf(suggestKind(entry)));

            if (!entry.comment.empty()) {
                item->setToolTip(ColumnName, QString::fromStdString(entry.comment));
            }
        };

    const QString text = m_search->text().trimmed();

    if (m_mode == Mode::ChooseMessage) {
        // Messages only: a flat list, narrowed by what is typed.
        QFont bold = m_tree->font();
        bold.setBold(true);

        for (const std::string& name : m_dictionary.messageNames()) {
            const QString shown = QString::fromStdString(name);

            if (!text.isEmpty() && !shown.contains(text, Qt::CaseInsensitive)) {
                continue;
            }

            auto* messageItem = new QTreeWidgetItem(m_tree);
            messageItem->setText(ColumnName, shown);
            messageItem->setFont(ColumnName, bold);
            messageItem->setData(ColumnName, kMessageRole, shown);

            const std::vector<const DictionaryEntry*> signalsOfIt = m_dictionary.signalsOf(name);
            messageItem->setText(ColumnUnit, QString::number(signalsOfIt.size()));

            if (!signalsOfIt.empty()) {
                if (signalsOfIt.front()->cycleTimeMs > 0) {
                    messageItem->setText(ColumnEvery,
                                         tr("%1 ms").arg(signalsOfIt.front()->cycleTimeMs));
                }

                messageItem->setText(ColumnRange,
                                     QString::fromStdString(signalsOfIt.front()->messageComment));
            }
        }

        if (m_tree->topLevelItemCount() > 0) {
            m_tree->setCurrentItem(m_tree->topLevelItem(0));
        }

        return;
    }

    if (text.isEmpty()) {
        // As the databases are organised: a message, and under it its signals.
        QFont bold = m_tree->font();
        bold.setBold(true);

        for (const std::string& name : m_dictionary.messageNames()) {
            auto* messageItem = new QTreeWidgetItem(m_tree);
            messageItem->setText(ColumnName, QString::fromStdString(name));
            messageItem->setFont(ColumnName, bold);
            messageItem->setData(ColumnName, kMessageRole, QString::fromStdString(name));

            // A message is chosen to make a table of it; asked for one signal, it is only a folder.
            if (m_mode == Mode::ChooseSignal) {
                messageItem->setFlags(messageItem->flags() & ~Qt::ItemIsSelectable);
            }

            const std::vector<const DictionaryEntry*> signalsOfIt = m_dictionary.signalsOf(name);
            messageItem->setText(ColumnUnit, QString::number(signalsOfIt.size()));

            for (const DictionaryEntry* entry : signalsOfIt) {
                addSignal(messageItem, *entry, false);
            }
        }

        // The first message open, so that a list of three messages is not a list of three rows.
        if (m_tree->topLevelItemCount() > 0 && m_tree->topLevelItemCount() <= 3) {
            m_tree->expandAll();
        }
    } else {
        // Ranked, so flat, and with the message in front of the name so that two signals of one
        // name can be told apart.
        for (const DictionaryEntry* entry : m_dictionary.search(text.toStdString(), kMostResults)) {
            addSignal(m_tree->invisibleRootItem(), *entry, true);
        }

        if (m_tree->topLevelItemCount() > 0) {
            m_tree->setCurrentItem(m_tree->topLevelItem(0));
        }
    }
}

void SignalPickerDialog::onSearchChanged()
{
    populate();
    onSelectionChanged();
}

void SignalPickerDialog::onSelectionChanged()
{
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();

    m_ok->setEnabled(!selected.isEmpty());

    // The one clicked last while it is still selected: with several chosen, the details are of the
    // one being looked at and not of whichever of them comes first in the list.
    QTreeWidgetItem* current = m_tree->currentItem();

    if (current == nullptr || !current->isSelected()) {
        current = selected.isEmpty() ? nullptr : selected.front();
    }

    if (const DictionaryEntry* entry = entryOf(current); entry != nullptr) {
        QString text = detailsOf(*entry);

        text += QLatin1Char('\n')
                + tr("\nMakes a %1 by default.").arg(displayNameOf(suggestKind(*entry)));
        m_details->setText(text);
        return;
    }

    if (current != nullptr && current->data(ColumnName, kMessageRole).isValid()) {
        const std::string name = current->data(ColumnName, kMessageRole).toString().toStdString();
        const std::vector<const DictionaryEntry*> signalsOfIt = m_dictionary.signalsOf(name);

        QString text = QString::fromStdString(name);

        if (!signalsOfIt.empty() && !signalsOfIt.front()->messageComment.empty()) {
            text += QLatin1Char('\n') + QString::fromStdString(signalsOfIt.front()->messageComment);
        }

        text += QLatin1String{"\n\n"}
                + tr("A table of this message lists its %n signal(s) with what each reads.",
                     nullptr,
                     static_cast<int>(signalsOfIt.size()));

        m_details->setText(text);
        return;
    }

    m_details->setText(m_dictionary.empty()
                           ? tr("No database is loaded. Import a .dbc in the DBC Explorer.")
                           : QString{});
}

void SignalPickerDialog::onItemDoubleClicked(QTreeWidgetItem* item)
{
    // A signal is chosen by double-clicking it, as a file is opened. A message is a folder, and
    // double-clicking it opens it - unless a message is what is being chosen.
    if (entryOf(item) != nullptr
        || (m_mode == Mode::ChooseMessage && item != nullptr
            && item->data(ColumnName, kMessageRole).isValid())) {
        accept();
    }
}

std::vector<DictionaryEntry> SignalPickerDialog::chosenSignals() const
{
    std::vector<std::size_t> positions;

    for (const QTreeWidgetItem* item : m_tree->selectedItems()) {
        if (const DictionaryEntry* entry = entryOf(item); entry != nullptr) {
            positions.push_back(static_cast<std::size_t>(entry - m_dictionary.entries().data()));
        }
    }

    // The databases' order, whatever order the clicks were in.
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());

    std::vector<DictionaryEntry> chosen;
    chosen.reserve(positions.size());

    for (const std::size_t position : positions) {
        chosen.push_back(m_dictionary.entries()[position]);
    }

    return chosen;
}

std::vector<std::string> SignalPickerDialog::chosenMessages() const
{
    std::vector<std::string> messages;

    if (m_mode != Mode::AddWidgets) {
        return messages;
    }

    for (const QTreeWidgetItem* item : m_tree->selectedItems()) {
        const QVariant name = item->data(ColumnName, kMessageRole);

        if (name.isValid()) {
            messages.push_back(name.toString().toStdString());
        }
    }

    return messages;
}

std::optional<DashboardWidgetKind> SignalPickerDialog::chosenKind() const
{
    if (m_kind == nullptr || m_kind->currentData().toInt() < 0) {
        return std::nullopt;
    }

    return static_cast<DashboardWidgetKind>(m_kind->currentData().toInt());
}

void SignalPickerDialog::setSearchText(const QString& text)
{
    m_search->setText(text);
}

int SignalPickerDialog::shownSignalCount() const
{
    int count = 0;

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* top = m_tree->topLevelItem(i);

        if (entryOf(top) != nullptr) {
            ++count;
        }

        for (int child = 0; child < top->childCount(); ++child) {
            if (entryOf(top->child(child)) != nullptr) {
                ++count;
            }
        }
    }

    return count;
}

bool SignalPickerDialog::select(std::string_view message, std::string_view signal)
{
    const auto matches = [&](QTreeWidgetItem* item) {
        if (signal.empty()) {
            return item->data(ColumnName, kMessageRole).toString().toStdString() == message;
        }

        const DictionaryEntry* entry = entryOf(item);

        return entry != nullptr && entry->message == message && entry->signal == signal;
    };

    QTreeWidgetItem* found = nullptr;

    for (int i = 0; i < m_tree->topLevelItemCount() && found == nullptr; ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);

        if (matches(top)) {
            found = top;
            break;
        }

        for (int child = 0; child < top->childCount(); ++child) {
            if (matches(top->child(child))) {
                found = top->child(child);
                break;
            }
        }
    }

    if (found == nullptr) {
        return false;
    }

    if (found->parent() != nullptr) {
        found->parent()->setExpanded(true);
    }

    // Not setCurrentItem(found) alone: what that does to the rest of the selection depends on the
    // modifier keys Qt believes are held, and a selection that is replaced or extended according to
    // a key nobody is pressing is not one to build on.
    m_tree->setCurrentItem(found, ColumnName, QItemSelectionModel::NoUpdate);
    found->setSelected(true);
    return true;
}

QString SignalPickerDialog::detailsText() const
{
    return m_details->text();
}

} // namespace torquebus::ui
