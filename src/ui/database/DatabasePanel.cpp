// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/database/DatabasePanel.h"

#include "core/database/DbcParser.h"
#include "ui/theme/ThemeManager.h"

#include <QAbstractItemView>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <cmath>

namespace torquebus::ui {
namespace {

enum Column : int {
    ColumnName = 0,
    ColumnPosition,
    ColumnRange,
    ColumnUnit,
    ColumnComment,
    ColumnCount
};

enum ItemKind : int { KindDatabase = 0, KindMessage, KindSignal };

constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;
constexpr int kMessageRole = Qt::UserRole + 3;
constexpr int kSignalRole = Qt::UserRole + 4;

/// The identifier in the form the rest of the application shows it: three hex
/// digits for standard, eight for extended. A trace that writes 0x101 and a
/// tree that writes 257 are describing the same message and do not look like
/// it.
[[nodiscard]] QString formatIdentifier(const CanMessage& message)
{
    return message.format == CanFrameFormat::Extended
               ? QStringLiteral("0x%1").arg(message.identifier, 8, 16, QLatin1Char('0')).toUpper()
               : QStringLiteral("0x%1").arg(message.identifier, 3, 16, QLatin1Char('0')).toUpper();
}

/// "0|12 @Motorola signed" - everything about where the bits are, in the width
/// of a column. The byte order is spelled out rather than left as @0 / @1,
/// because @0 meaning big-endian is a piece of Vector trivia and this panel is
/// where somebody is trying to understand a database, not write one.
[[nodiscard]] QString formatPosition(const CanSignal& signal)
{
    return QStringLiteral("%1|%2 %3 %4")
        .arg(signal.startBit)
        .arg(signal.bitLength)
        .arg(signal.byteOrder == ByteOrder::Motorola ? QStringLiteral("Motorola")
                                                     : QStringLiteral("Intel"))
        .arg(signal.isSigned ? QStringLiteral("signed") : QStringLiteral("unsigned"));
}

/// The scaling, only when there is one. A signal with factor 1 and offset 0 is
/// most signals, and printing "×1 +0" on every row of a long list is noise that
/// hides the rows where the scaling actually matters.
[[nodiscard]] QString formatScaling(const CanSignal& signal)
{
    if (signal.factor == 1.0 && signal.offset == 0.0) {
        return {};
    }

    QString text = QStringLiteral("x%1").arg(signal.factor, 0, 'g', 10);
    if (signal.offset != 0.0) {
        text += QStringLiteral(" %1%2")
                    .arg(signal.offset < 0 ? QStringLiteral("-") : QStringLiteral("+"))
                    .arg(std::abs(signal.offset), 0, 'g', 10);
    }
    return text;
}

[[nodiscard]] QString formatRange(const CanSignal& signal)
{
    if (signal.minimum == 0.0 && signal.maximum == 0.0) {
        return formatScaling(signal);
    }

    QString text =
        QStringLiteral("%1 .. %2").arg(signal.minimum, 0, 'g', 10).arg(signal.maximum, 0, 'g', 10);

    if (const QString scaling = formatScaling(signal); !scaling.isEmpty()) {
        text += QStringLiteral("  (%1)").arg(scaling);
    }
    return text;
}

/// Multiplexing, in the words a person would use. "m3" is compact and means
/// nothing to somebody who has not read the DBC specification.
[[nodiscard]] QString formatMultiplexing(const CanSignal& signal)
{
    if (signal.isMultiplexer && signal.multiplexerValue.has_value()) {
        return DatabasePanel::tr("switch, and present when it reads %1")
            .arg(*signal.multiplexerValue);
    }
    if (signal.isMultiplexer) {
        return DatabasePanel::tr("multiplexer switch");
    }
    if (signal.multiplexerValue.has_value()) {
        return DatabasePanel::tr("present when the switch reads %1").arg(*signal.multiplexerValue);
    }
    return {};
}

} // namespace

DatabasePanel::DatabasePanel(QWidget* parent)
    : QWidget{parent}
{
    buildUi();

    connect(m_filter, &QLineEdit::textChanged, this, &DatabasePanel::onFilterChanged);
    connect(m_tree,
            &QTreeWidget::currentItemChanged,
            this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { onCurrentItemChanged(current); });

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }

    updateSummary();
}

void DatabasePanel::buildUi()
{
    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(tr("Filter messages and signals"));

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("Name"), tr("Position"), tr("Range"), tr("Unit"), tr("Comment")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(ColumnName, QHeaderView::Interactive);
    m_tree->setColumnWidth(ColumnName, 220);
    m_tree->setColumnWidth(ColumnPosition, 170);
    m_tree->setColumnWidth(ColumnRange, 170);
    m_tree->setColumnWidth(ColumnUnit, 60);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("panelStatus"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 4);
    layout->setSpacing(4);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_summary);
}

bool DatabasePanel::loadDatabase(const QString& path)
{
    const QString normalizedNew = QFileInfo{path}.absoluteFilePath();
    for (const std::shared_ptr<CanDatabase>& existing : m_databases) {
        if (!existing) {
            continue;
        }
        const QString existingAbs =
            QFileInfo{QString::fromStdString(existing->sourcePath)}.absoluteFilePath();
        if (!normalizedNew.isEmpty()
            && QString::compare(existingAbs, normalizedNew, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    auto database = std::make_shared<CanDatabase>();

    const Result result = DbcParser::parseFile(path.toStdString(), *database);
    if (result.failed()) {
        Q_EMIT databaseFailed(path, QString::fromStdString(std::string{result.message()}));
        return false;
    }

    const int messages = static_cast<int>(database->messageCount());
    const int signalTotal = static_cast<int>(database->signalCount());

    addDatabaseToTree(*database);
    m_databases.push_back(std::move(database));

    applyIcons();
    updateSummary();
    onFilterChanged(m_filter->text());

    Q_EMIT databaseLoaded(path, messages, signalTotal);
    return true;
}

void DatabasePanel::addDatabaseToTree(const CanDatabase& database)
{
    const QString path = QString::fromStdString(database.sourcePath);

    auto* root = new QTreeWidgetItem(m_tree);
    root->setText(ColumnName, QFileInfo{path}.fileName());
    root->setText(ColumnComment, path);
    root->setToolTip(ColumnName, path);
    root->setData(0, kKindRole, KindDatabase);
    root->setData(0, kPathRole, path);
    root->setExpanded(true);

    if (!database.version.empty()) {
        root->setText(ColumnRange, tr("version %1").arg(QString::fromStdString(database.version)));
    }

    for (const CanMessage& message : database.messages()) {
        auto* messageItem = new QTreeWidgetItem(root);
        messageItem->setText(ColumnName, QString::fromStdString(message.name));
        messageItem->setText(ColumnPosition, formatIdentifier(message));
        messageItem->setText(ColumnRange, tr("%n byte(s)", nullptr, message.length));
        messageItem->setText(ColumnComment, QString::fromStdString(message.comment));
        messageItem->setData(0, kKindRole, KindMessage);
        messageItem->setData(0, kPathRole, path);
        messageItem->setData(0, kMessageRole, QString::fromStdString(message.name));

        if (message.cycleTimeMs > 0) {
            messageItem->setText(ColumnUnit, tr("%1 ms").arg(message.cycleTimeMs));
        }

        if (!message.transmitter.empty()) {
            messageItem->setToolTip(
                ColumnName, tr("Sent by %1").arg(QString::fromStdString(message.transmitter)));
        }

        for (const CanSignal& signal : message.signalList) {
            auto* signalItem = new QTreeWidgetItem(messageItem);
            signalItem->setText(ColumnName, QString::fromStdString(signal.name));
            signalItem->setText(ColumnPosition, formatPosition(signal));
            signalItem->setText(ColumnRange, formatRange(signal));
            signalItem->setText(ColumnUnit, QString::fromStdString(signal.unit));
            signalItem->setData(0, kKindRole, KindSignal);
            signalItem->setData(0, kPathRole, path);
            signalItem->setData(0, kMessageRole, QString::fromStdString(message.name));
            signalItem->setData(0, kSignalRole, QString::fromStdString(signal.name));

            // The comment column carries whichever of the three things the
            // signal actually has. A comment is the most useful, then the
            // multiplexing, then the value table - and a row that says all
            // three is a row nobody reads.
            QString detail = QString::fromStdString(signal.comment);
            if (detail.isEmpty()) {
                detail = formatMultiplexing(signal);
            }
            if (detail.isEmpty() && !signal.valueNames.empty()) {
                QStringList names;
                for (const SignalValueName& value : signal.valueNames) {
                    names << QStringLiteral("%1 = %2")
                                 .arg(value.value)
                                 .arg(QString::fromStdString(value.name));
                }
                detail = names.join(QStringLiteral(", "));
            }
            signalItem->setText(ColumnComment, detail);

            // The full value table goes in the tooltip whatever the column
            // shows, because a gear selector with sixteen positions is exactly
            // the signal whose comment is also worth having.
            if (!signal.valueNames.empty()) {
                QStringList names;
                for (const SignalValueName& value : signal.valueNames) {
                    names << QStringLiteral("%1 = %2")
                                 .arg(value.value)
                                 .arg(QString::fromStdString(value.name));
                }
                signalItem->setToolTip(ColumnComment, names.join(QLatin1Char('\n')));
            }
        }
    }
}

void DatabasePanel::clear()
{
    m_tree->clear();
    m_databases.clear();
    updateSummary();
}

std::vector<std::shared_ptr<const CanDatabase>> DatabasePanel::databases() const
{
    return {m_databases.begin(), m_databases.end()};
}

int DatabasePanel::databaseCount() const
{
    return static_cast<int>(m_databases.size());
}

QStringList DatabasePanel::databasePaths() const
{
    QStringList paths;
    paths.reserve(static_cast<qsizetype>(m_databases.size()));
    for (const std::shared_ptr<CanDatabase>& db : m_databases) {
        if (db && !db->sourcePath.empty()) {
            paths.push_back(QString::fromStdString(db->sourcePath));
        }
    }
    return paths;
}

void DatabasePanel::updateSummary()
{
    if (m_databases.empty()) {
        m_summary->setText(tr("No database loaded. File → Import Database..."));
        return;
    }

    std::size_t messages = 0;
    std::size_t signalTotal = 0;
    for (const std::shared_ptr<CanDatabase>& database : m_databases) {
        messages += database->messageCount();
        signalTotal += database->signalCount();
    }

    m_summary->setText(tr("%1 database(s), %2 message(s), %3 signal(s)")
                           .arg(m_databases.size())
                           .arg(messages)
                           .arg(signalTotal));
}

void DatabasePanel::onFilterChanged(const QString& text)
{
    const QString needle = text.trimmed();

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        (void)applyFilter(m_tree->topLevelItem(i), needle);
    }
}

bool DatabasePanel::applyFilter(QTreeWidgetItem* item, const QString& needle)
{
    // Depth first: a message stays visible when one of its signals matches,
    // which is the whole point of filtering a tree rather than a list. Asking
    // the parent first would hide the children that were the reason to keep it.
    bool anyChildMatches = false;
    for (int i = 0; i < item->childCount(); ++i) {
        anyChildMatches = applyFilter(item->child(i), needle) || anyChildMatches;
    }

    const bool selfMatches = needle.isEmpty()
                             || item->text(ColumnName).contains(needle, Qt::CaseInsensitive)
                             || item->text(ColumnPosition).contains(needle, Qt::CaseInsensitive);

    const bool visible = selfMatches || anyChildMatches;
    item->setHidden(!visible);

    // Expand to reveal a match, but do not collapse anything on the way back to
    // an empty filter: the user's own expansion state is theirs.
    if (!needle.isEmpty() && anyChildMatches) {
        item->setExpanded(true);
    }

    return visible;
}

void DatabasePanel::onCurrentItemChanged(QTreeWidgetItem* current)
{
    if (current == nullptr || current->data(0, kKindRole).toInt() != KindSignal) {
        return;
    }

    Q_EMIT signalSelected(current->data(0, kPathRole).toString(),
                          current->data(0, kMessageRole).toString(),
                          current->data(0, kSignalRole).toString());
}

void DatabasePanel::applyIcons()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr) {
        return;
    }

    const QIcon database = themes->icon(QStringLiteral("database"));
    const QIcon trace = themes->icon(QStringLiteral("trace"));

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = m_tree->topLevelItem(i);
        root->setIcon(ColumnName, database);

        for (int j = 0; j < root->childCount(); ++j) {
            root->child(j)->setIcon(ColumnName, trace);
        }
    }
}

void DatabasePanel::onThemeChanged()
{
    // Icons are monochrome SVGs tinted at load time, so one made under the dark
    // theme is a pale glyph that vanishes on a light window.
    applyIcons();
}

} // namespace torquebus::ui
