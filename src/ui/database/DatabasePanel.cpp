// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/database/DatabasePanel.h"

#include "core/database/DbcParser.h"
#include "ui/dashboard/SignalMime.h"
#include "ui/theme/ThemeManager.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStringList>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <vector>

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
constexpr int kMultiplexRole = Qt::UserRole + 5;
constexpr int kCardTitleRole = Qt::UserRole + 6;
constexpr int kCardMetaRole = Qt::UserRole + 7;
constexpr int kCardBodyRole = Qt::UserRole + 8;
constexpr int kCardValuesRole = Qt::UserRole + 9;

constexpr int kCommentCardLineWidth = 54;

/// Where an item is in the tree: its row under each of its parents, from the top. Items sorted by
/// it are items in the order the tree shows them.
[[nodiscard]] std::vector<int> pathOf(QTreeWidgetItem* item)
{
    std::vector<int> path;

    for (QTreeWidgetItem* at = item; at != nullptr; at = at->parent()) {
        const QTreeWidgetItem* parent = at->parent();

        path.push_back(parent != nullptr ? parent->indexOfChild(at)
                                         : at->treeWidget()->indexOfTopLevelItem(at));
    }

    std::reverse(path.begin(), path.end());
    return path;
}

/// What an item of the tree is when it is dragged: a signal, or a message as a whole. A database is
/// a file, and the Dashboard has nothing to make of one.
///
/// In the order the tree shows them. The selection keeps its items in the order of its own ranges,
/// which is not the order anybody sees, and widgets made from a drag are laid out in the order they
/// are given: three signals picked from the top of a message should not come out as the third, the
/// first and the second.
[[nodiscard]] QList<SignalReference> referencesOf(const QList<QTreeWidgetItem*>& items)
{
    QList<QTreeWidgetItem*> inOrder = items;

    std::stable_sort(inOrder.begin(), inOrder.end(), [](QTreeWidgetItem* a, QTreeWidgetItem* b) {
        return pathOf(a) < pathOf(b);
    });

    QList<SignalReference> references;

    for (const QTreeWidgetItem* item : inOrder) {
        const int kind = item->data(0, kKindRole).toInt();

        if (kind != KindSignal && kind != KindMessage) {
            continue;
        }

        SignalReference reference;
        reference.message = item->data(0, kMessageRole).toString();

        if (kind == KindSignal) {
            reference.signal = item->data(0, kSignalRole).toString();
        }

        if (!references.contains(reference)) {
            references.append(reference);
        }
    }

    return references;
}

/// The tree, which can be dragged from: onto the Dashboard, to make a widget of a signal or a
/// table of a message. Nothing can be dropped on it; the explorer shows what a file says and does
/// not edit it.
class ExplorerTree final : public QTreeWidget {
public:
    explicit ExplorerTree(QWidget* parent)
        : QTreeWidget{parent}
    {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
    }

protected:
    [[nodiscard]] QStringList mimeTypes() const override
    {
        return {QString::fromLatin1(kSignalMimeType)};
    }

    [[nodiscard]] QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        const QList<SignalReference> references = referencesOf(items);

        // Nothing a drop could use: a drag of only a file name starts no drag at all.
        return references.isEmpty() ? nullptr : makeSignalMimeData(references);
    }
};

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

/// Wraps a description at word boundaries so the hover card and tooltip present
/// long comments across multiple balanced lines rather than as a single wide ribbon.
[[nodiscard]] QString wrapCommentLines(const QString& text,
                                       int maxLineChars = kCommentCardLineWidth)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QStringList wrappedParagraphs;
    const QStringList paragraphs = trimmed.split(QLatin1Char('\n'));

    for (const QString& rawParagraph : paragraphs) {
        const QString paragraph = rawParagraph.simplified();
        if (paragraph.isEmpty()) {
            continue;
        }
        if (paragraph.size() <= maxLineChars) {
            wrappedParagraphs << paragraph;
            continue;
        }

        const QStringList words = paragraph.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QString currentLine;
        for (const QString& word : words) {
            if (currentLine.isEmpty()) {
                currentLine = word;
            } else if (currentLine.size() + 1 + word.size() <= maxLineChars) {
                currentLine += QLatin1Char(' ') + word;
            } else {
                wrappedParagraphs << currentLine;
                currentLine = word;
            }
        }
        if (!currentLine.isEmpty()) {
            wrappedParagraphs << currentLine;
        }
    }

    return wrappedParagraphs.join(QLatin1Char('\n'));
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

    m_removeButton = new QPushButton(tr("Remove Database"), this);
    m_removeButton->setObjectName(QStringLiteral("removeDatabaseButton"));
    m_removeButton->setToolTip(
        tr("Remove the selected database from the project (save the project to persist removal)"));
    m_removeButton->setEnabled(false);
    connect(
        m_removeButton, &QPushButton::clicked, this, [this] { (void)removeSelectedDatabase(); });

    auto* topBar = new QHBoxLayout;
    topBar->setContentsMargins(0, 0, 0, 0);
    topBar->setSpacing(4);
    topBar->addWidget(m_filter, 1);
    topBar->addWidget(m_removeButton);

    m_tree = new ExplorerTree(this);
    m_tree->setColumnCount(ColumnCount);
    m_tree->setHeaderLabels({tr("Name"), tr("Position"), tr("Range"), tr("Unit"), tr("Comment")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setMouseTracking(true);
    m_tree->viewport()->setMouseTracking(true);
    m_tree->viewport()->installEventFilter(this);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_tree->itemAt(pos);
        if (item == nullptr && m_databases.empty()) {
            return;
        }

        QMenu menu(this);
        ThemeManager* themes = ThemeManager::instance();
        const auto icon = [themes](const char* name) {
            return themes != nullptr ? themes->icon(QString::fromLatin1(name)) : QIcon{};
        };

        if (item != nullptr) {
            const int kind = item->data(0, kKindRole).toInt();
            if (kind == KindMessage) {
                const QString msgName = item->data(0, kMessageRole).toString();
                const QString dbPath = item->data(0, kPathRole).toString();

                const CanMessage* foundMsg = nullptr;
                for (const auto& db : m_databases) {
                    if (QString::fromStdString(db->sourcePath) == dbPath) {
                        for (const auto& msg : db->messages()) {
                            if (QString::fromStdString(msg.name) == msgName) {
                                foundMsg = &msg;
                                break;
                            }
                        }
                    }
                }

                QAction* title = menu.addAction(tr("Message: %1").arg(msgName));
                title->setEnabled(false);
                menu.addSeparator();

                if (foundMsg != nullptr) {
                    QAction* addTx = menu.addAction(icon("transmit"), tr("Add to Transmit List"));
                    connect(addTx, &QAction::triggered, this, [this, foundMsg] {
                        Q_EMIT sendToTransmitRequested(*foundMsg);
                    });

                    QAction* plotAll = menu.addAction(icon("graph"), tr("Plot All Signals in Graph"));
                    connect(plotAll, &QAction::triggered, this, [this, foundMsg] {
                        for (const auto& sig : foundMsg->signalList) {
                            Q_EMIT plotSignalRequested(QString::fromStdString(sig.name));
                        }
                    });
                }

                QAction* copyName = menu.addAction(tr("Copy Message Name"));
                connect(copyName, &QAction::triggered, this, [msgName] {
                    if (QClipboard* clip = QGuiApplication::clipboard()) {
                        clip->setText(msgName);
                    }
                });

                if (foundMsg != nullptr) {
                    const QString hexId =
                        QStringLiteral("0x%1").arg(foundMsg->identifier, 0, 16).toUpper();
                    QAction* copyId = menu.addAction(tr("Copy Identifier (%1)").arg(hexId));
                    connect(copyId, &QAction::triggered, this, [hexId] {
                        if (QClipboard* clip = QGuiApplication::clipboard()) {
                            clip->setText(hexId);
                        }
                    });
                }
            } else if (kind == KindSignal) {
                const QString sigName = item->data(0, kSignalRole).toString();
                const QString msgName = item->data(0, kMessageRole).toString();
                const QString dbPath = item->data(0, kPathRole).toString();

                QAction* title = menu.addAction(tr("Signal: %1").arg(sigName));
                title->setEnabled(false);
                menu.addSeparator();

                QAction* plot = menu.addAction(icon("graph"), tr("Plot in Graph"));
                connect(plot, &QAction::triggered, this, [this, sigName] {
                    Q_EMIT plotSignalRequested(sigName);
                });

                const CanMessage* foundMsg = nullptr;
                for (const auto& db : m_databases) {
                    if (QString::fromStdString(db->sourcePath) == dbPath) {
                        for (const auto& msg : db->messages()) {
                            if (QString::fromStdString(msg.name) == msgName) {
                                foundMsg = &msg;
                                break;
                            }
                        }
                    }
                }

                if (foundMsg != nullptr) {
                    QAction* addTx = menu.addAction(
                        icon("transmit"), tr("Add Message '%1' to Transmit List").arg(msgName));
                    connect(addTx, &QAction::triggered, this, [this, foundMsg] {
                        Q_EMIT sendToTransmitRequested(*foundMsg);
                    });
                }

                QAction* copyName = menu.addAction(tr("Copy Signal Name"));
                connect(copyName, &QAction::triggered, this, [sigName] {
                    if (QClipboard* clip = QGuiApplication::clipboard()) {
                        clip->setText(sigName);
                    }
                });
            } else if (kind == KindDatabase) {
                const QString targetPath = item->data(0, kPathRole).toString();
                QAction* removeAction = menu.addAction(
                    icon("clear"),
                    tr("Remove '%1' from Project").arg(QFileInfo{targetPath}.fileName()));
                connect(removeAction, &QAction::triggered, this, [this, targetPath] {
                    (void)removeDatabase(targetPath);
                });
            }
        } else if (!m_databases.empty()) {
            const QString targetPath =
                m_databases.size() == 1 ? QString::fromStdString(m_databases.front()->sourcePath)
                                        : QString{};
            if (!targetPath.isEmpty()) {
                QAction* removeAction = menu.addAction(
                    icon("clear"),
                    tr("Remove '%1' from Project").arg(QFileInfo{targetPath}.fileName()));
                connect(removeAction, &QAction::triggered, this, [this, targetPath] {
                    (void)removeDatabase(targetPath);
                });
            }
        }

        if (!menu.actions().isEmpty()) {
            menu.exec(m_tree->viewport()->mapToGlobal(pos));
        }
    });

    // Several at once, so that the signals of one message can be dragged to the Dashboard together.
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(ColumnName, QHeaderView::Interactive);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setColumnWidth(ColumnName, 190);
    m_tree->setColumnWidth(ColumnPosition, 90);
    m_tree->setColumnWidth(ColumnRange, 70);
    m_tree->setColumnWidth(ColumnUnit, 60);

    connect(m_tree->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        hideCommentCard();
    });
    connect(m_tree->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        hideCommentCard();
    });

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("panelStatus"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 4);
    layout->setSpacing(4);
    layout->addLayout(topBar);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_summary);

    buildCommentCard();
    applyIcons();
}

void DatabasePanel::buildCommentCard()
{
    m_commentCard = new QFrame(this,
                               Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput
                                   | Qt::WindowDoesNotAcceptFocus);
    m_commentCard->setObjectName(QStringLiteral("dbcCommentCard"));
    m_commentCard->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_commentCard->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_commentCard->setMinimumWidth(300);
    m_commentCard->setMaximumWidth(420);

    auto* cardLayout = new QVBoxLayout(m_commentCard);
    cardLayout->setContentsMargins(10, 8, 10, 8);
    cardLayout->setSpacing(4);

    m_cardTitle = new QLabel(m_commentCard);
    m_cardTitle->setObjectName(QStringLiteral("dbcCommentCardTitle"));
    m_cardTitle->setWordWrap(true);
    m_cardTitle->setTextFormat(Qt::PlainText);

    m_cardMeta = new QLabel(m_commentCard);
    m_cardMeta->setObjectName(QStringLiteral("dbcCommentCardMeta"));
    m_cardMeta->setWordWrap(true);
    m_cardMeta->setTextFormat(Qt::PlainText);

    m_cardBody = new QLabel(m_commentCard);
    m_cardBody->setObjectName(QStringLiteral("dbcCommentCardBody"));
    m_cardBody->setWordWrap(true);
    m_cardBody->setTextFormat(Qt::PlainText);

    m_cardValues = new QLabel(m_commentCard);
    m_cardValues->setObjectName(QStringLiteral("dbcCommentCardValues"));
    m_cardValues->setWordWrap(true);
    m_cardValues->setTextFormat(Qt::PlainText);

    cardLayout->addWidget(m_cardTitle);
    cardLayout->addWidget(m_cardMeta);
    cardLayout->addWidget(m_cardBody);
    cardLayout->addWidget(m_cardValues);

    m_commentCard->hide();
}

void DatabasePanel::updateCommentCardAt(const QPoint& viewportPos)
{
    if (m_tree == nullptr || m_commentCard == nullptr) {
        return;
    }

    QTreeWidgetItem* item = m_tree->itemAt(viewportPos);
    const int column = m_tree->columnAt(viewportPos.x());
    if (item == nullptr || column != ColumnComment) {
        hideCommentCard();
        return;
    }

    const QString title = item->data(0, kCardTitleRole).toString();
    const QString meta = item->data(0, kCardMetaRole).toString();
    const QString body = item->data(0, kCardBodyRole).toString();
    const QString values = item->data(0, kCardValuesRole).toString();
    if (body.isEmpty() && values.isEmpty()) {
        hideCommentCard();
        return;
    }

    if (m_hoveredCommentItem != item) {
        m_hoveredCommentItem = item;

        m_cardTitle->setText(title);
        m_cardTitle->setVisible(!title.isEmpty());

        m_cardMeta->setText(meta);
        m_cardMeta->setVisible(!meta.isEmpty());

        if (!body.isEmpty()) {
            m_cardBody->setText(body);
            m_cardBody->setVisible(true);
            m_cardValues->setText(values);
            m_cardValues->setVisible(!values.isEmpty());
        } else {
            m_cardBody->setText(values);
            m_cardBody->setVisible(true);
            m_cardValues->clear();
            m_cardValues->setVisible(false);
        }

        if (m_commentCard->layout() != nullptr) {
            m_commentCard->layout()->activate();
        }
        m_commentCard->adjustSize();
    }

    const QModelIndex index = m_tree->indexFromItem(item, ColumnComment);
    const QRect cellRect = m_tree->visualRect(index);
    const QPoint globalCursor = m_tree->viewport()->mapToGlobal(viewportPos);
    const QPoint cellBottomLeft = m_tree->viewport()->mapToGlobal(cellRect.bottomLeft());
    const QPoint cellTopLeft = m_tree->viewport()->mapToGlobal(cellRect.topLeft());

    const QSize cardSize = m_commentCard->sizeHint().expandedTo(m_commentCard->minimumSize());
    int x = std::max(cellBottomLeft.x(), globalCursor.x() - 24);
    int y = cellBottomLeft.y() + 6;

    if (const QScreen* screen = QGuiApplication::screenAt(globalCursor)) {
        const QRect avail = screen->availableGeometry();
        if (x + cardSize.width() > avail.right() - 8) {
            x = std::max(avail.left() + 8, avail.right() - cardSize.width() - 8);
        }
        if (y + cardSize.height() > avail.bottom() - 8) {
            y = std::max(avail.top() + 8, cellTopLeft.y() - cardSize.height() - 6);
        }
    }

    m_commentCard->move(x, y);
    if (!m_commentCard->isVisible()) {
        m_commentCard->show();
    }
}

void DatabasePanel::hideCommentCard()
{
    m_hoveredCommentItem = nullptr;
    if (m_commentCard != nullptr && m_commentCard->isVisible()) {
        m_commentCard->hide();
    }
}

bool DatabasePanel::eventFilter(QObject* watched, QEvent* event)
{
    if (m_tree != nullptr && watched == m_tree->viewport() && event != nullptr) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            updateCommentCardAt(mouseEvent->pos());
            break;
        }
        case QEvent::ToolTip: {
            const auto* helpEvent = static_cast<QHelpEvent*>(event);
            if (m_tree->columnAt(helpEvent->pos().x()) == ColumnComment) {
                updateCommentCardAt(helpEvent->pos());
                QToolTip::hideText();
                return true;
            }
            hideCommentCard();
            break;
        }
        case QEvent::Leave:
        case QEvent::Wheel:
        case QEvent::Hide:
            hideCommentCard();
            break;
        default:
            break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void DatabasePanel::hideEvent(QHideEvent* event)
{
    hideCommentCard();
    QWidget::hideEvent(event);
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
    const QString fileName = QFileInfo{path}.fileName();

    auto* root = new QTreeWidgetItem(m_tree);
    root->setText(ColumnName, fileName);
    root->setText(ColumnComment, path);
    root->setToolTip(ColumnName, path);
    root->setData(0, kKindRole, KindDatabase);
    root->setData(0, kPathRole, path);
    root->setExpanded(true);

    QStringList rootMetaParts;
    if (!database.version.empty()) {
        const QString versionText = tr("version %1").arg(QString::fromStdString(database.version));
        root->setText(ColumnRange, versionText);
        rootMetaParts << versionText;
    }
    rootMetaParts << tr("%1 message(s), %2 signal(s)")
                         .arg(database.messageCount())
                         .arg(database.signalCount());

    const QString wrappedPath = wrapCommentLines(path);
    root->setData(0, kCardTitleRole, fileName);
    root->setData(0, kCardMetaRole, rootMetaParts.join(QStringLiteral(" · ")));
    root->setData(0, kCardBodyRole, wrappedPath);
    root->setToolTip(ColumnComment, wrappedPath);

    for (const CanMessage& message : database.messages()) {
        const QString messageName = QString::fromStdString(message.name);
        const QString identifierText = formatIdentifier(message);
        const QString lengthText = tr("%n byte(s)", nullptr, message.length);
        const QString messageComment = QString::fromStdString(message.comment);

        auto* messageItem = new QTreeWidgetItem(root);
        messageItem->setText(ColumnName, messageName);
        messageItem->setText(ColumnPosition, identifierText);
        messageItem->setText(ColumnRange, lengthText);
        messageItem->setText(ColumnComment, messageComment);
        messageItem->setData(0, kKindRole, KindMessage);
        messageItem->setData(0, kPathRole, path);
        messageItem->setData(0, kMessageRole, messageName);

        QStringList messageMetaParts{lengthText};
        if (message.cycleTimeMs > 0) {
            const QString cycleText = tr("%1 ms").arg(message.cycleTimeMs);
            messageItem->setText(ColumnUnit, cycleText);
            messageMetaParts << cycleText;
        }

        if (!message.transmitter.empty()) {
            const QString txText =
                tr("Sent by %1").arg(QString::fromStdString(message.transmitter));
            messageItem->setToolTip(ColumnName, txText);
            messageMetaParts << txText;
        }

        const QString wrappedMessageComment = wrapCommentLines(messageComment);
        messageItem->setData(
            0, kCardTitleRole, QStringLiteral("%1 (%2)").arg(messageName, identifierText));
        messageItem->setData(0, kCardMetaRole, messageMetaParts.join(QStringLiteral(" · ")));
        messageItem->setData(0, kCardBodyRole, wrappedMessageComment);
        if (!wrappedMessageComment.isEmpty()) {
            messageItem->setToolTip(ColumnComment, wrappedMessageComment);
        }

        for (const CanSignal& signal : message.signalList) {
            const QString signalName = QString::fromStdString(signal.name);
            const QString positionText = formatPosition(signal);
            const QString rangeText = formatRange(signal);
            const QString unitText = QString::fromStdString(signal.unit);

            auto* signalItem = new QTreeWidgetItem(messageItem);
            signalItem->setText(ColumnName, signalName);
            signalItem->setText(ColumnPosition, positionText);
            signalItem->setText(ColumnRange, rangeText);
            signalItem->setText(ColumnUnit, unitText);
            signalItem->setData(0, kKindRole, KindSignal);
            signalItem->setData(0, kPathRole, path);
            signalItem->setData(0, kMessageRole, messageName);
            signalItem->setData(0, kSignalRole, signalName);
            signalItem->setData(
                0, kMultiplexRole, signal.isMultiplexer || signal.multiplexerValue.has_value());

            const QString signalComment = QString::fromStdString(signal.comment);
            const QString multiplexingText = formatMultiplexing(signal);
            QStringList valueLines;
            for (const SignalValueName& value : signal.valueNames) {
                valueLines << QStringLiteral("%1 = %2")
                                  .arg(value.value)
                                  .arg(QString::fromStdString(value.name));
            }

            // The comment column carries whichever of the three things the
            // signal actually has. A comment is the most useful, then the
            // multiplexing, then the value table - and a row that says all
            // three is a row nobody reads.
            QString detail = signalComment;
            if (detail.isEmpty()) {
                detail = multiplexingText;
            }
            if (detail.isEmpty() && !valueLines.isEmpty()) {
                detail = valueLines.join(QStringLiteral(", "));
            }
            signalItem->setText(ColumnComment, detail);

            QStringList signalMetaParts{positionText};
            if (!rangeText.isEmpty()) {
                signalMetaParts << (unitText.isEmpty()
                                        ? rangeText
                                        : QStringLiteral("%1 %2").arg(rangeText, unitText));
            } else if (!unitText.isEmpty()) {
                signalMetaParts << unitText;
            }

            const QString rawCardBody = !signalComment.isEmpty() ? signalComment : multiplexingText;
            const QString wrappedSignalBody = wrapCommentLines(rawCardBody);

            QStringList extraCardLines;
            if (!signalComment.isEmpty() && !multiplexingText.isEmpty()) {
                extraCardLines << multiplexingText;
            }
            extraCardLines.append(valueLines);
            const QString cardValuesText = extraCardLines.join(QLatin1Char('\n'));

            signalItem->setData(
                0, kCardTitleRole, QStringLiteral("%1 · %2").arg(messageName, signalName));
            signalItem->setData(0, kCardMetaRole, signalMetaParts.join(QStringLiteral(" · ")));
            signalItem->setData(0, kCardBodyRole, wrappedSignalBody);
            signalItem->setData(0, kCardValuesRole, cardValuesText);

            QStringList toolTipSections;
            if (!wrappedSignalBody.isEmpty()) {
                toolTipSections << wrappedSignalBody;
            }
            if (!cardValuesText.isEmpty()) {
                toolTipSections << cardValuesText;
            }
            if (!toolTipSections.isEmpty()) {
                signalItem->setToolTip(ColumnComment, toolTipSections.join(QStringLiteral("\n\n")));
            }
        }
    }
}

bool DatabasePanel::removeDatabase(const QString& path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    const QString normalizedTarget = QFileInfo{trimmed}.absoluteFilePath();
    auto matchesPath = [&](const QString& candidate) {
        if (QString::compare(candidate, trimmed, Qt::CaseInsensitive) == 0) {
            return true;
        }
        const QString normalizedCandidate = QFileInfo{candidate}.absoluteFilePath();
        return !normalizedTarget.isEmpty()
               && QString::compare(normalizedCandidate, normalizedTarget, Qt::CaseInsensitive) == 0;
    };

    QString removedPath;
    for (auto it = m_databases.begin(); it != m_databases.end(); ++it) {
        if (!*it) {
            continue;
        }
        const QString candidate = QString::fromStdString((*it)->sourcePath);
        if (matchesPath(candidate)) {
            removedPath = candidate;
            m_databases.erase(it);
            break;
        }
    }

    if (removedPath.isEmpty()) {
        return false;
    }

    hideCommentCard();

    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* root = m_tree->topLevelItem(i);
        if (root != nullptr && matchesPath(root->data(0, kPathRole).toString())) {
            delete m_tree->takeTopLevelItem(i);
        }
    }

    updateSummary();
    Q_EMIT databaseRemoved(removedPath);
    return true;
}

bool DatabasePanel::removeSelectedDatabase()
{
    if (m_databases.empty() || m_tree == nullptr) {
        return false;
    }

    QStringList pathsToRemove;
    for (const QTreeWidgetItem* item : m_tree->selectedItems()) {
        if (item == nullptr) {
            continue;
        }
        const QString itemPath = item->data(0, kPathRole).toString();
        if (!itemPath.isEmpty() && !pathsToRemove.contains(itemPath, Qt::CaseInsensitive)) {
            pathsToRemove.push_back(itemPath);
        }
    }

    if (pathsToRemove.isEmpty() && m_tree->currentItem() != nullptr) {
        const QString currentPath = m_tree->currentItem()->data(0, kPathRole).toString();
        if (!currentPath.isEmpty()) {
            pathsToRemove.push_back(currentPath);
        }
    }

    if (pathsToRemove.isEmpty() && !m_databases.empty()) {
        pathsToRemove.push_back(QString::fromStdString(m_databases.back()->sourcePath));
    }

    bool anyRemoved = false;
    for (const QString& targetPath : std::as_const(pathsToRemove)) {
        anyRemoved = removeDatabase(targetPath) || anyRemoved;
    }
    return anyRemoved;
}

void DatabasePanel::clear()
{
    hideCommentCard();
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
    if (m_removeButton != nullptr) {
        m_removeButton->setEnabled(!m_databases.empty());
    }

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
    hideCommentCard();
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

    if (m_removeButton != nullptr) {
        m_removeButton->setIcon(themes->icon(QStringLiteral("clear")));
    }

    const QIcon databaseIcon = themes->icon(QStringLiteral("database"));
    const QIcon messageIcon = themes->icon(QStringLiteral("message"));
    const QIcon signalIcon = themes->icon(QStringLiteral("signal"));
    const QIcon signalMuxIcon = themes->icon(QStringLiteral("signal-multiplex"));

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = m_tree->topLevelItem(i);
        root->setIcon(ColumnName, databaseIcon);

        for (int j = 0; j < root->childCount(); ++j) {
            QTreeWidgetItem* messageItem = root->child(j);
            messageItem->setIcon(ColumnName, messageIcon);

            for (int k = 0; k < messageItem->childCount(); ++k) {
                QTreeWidgetItem* signalItem = messageItem->child(k);
                const bool isMux = signalItem->data(0, kMultiplexRole).toBool();
                signalItem->setIcon(ColumnName, isMux ? signalMuxIcon : signalIcon);
            }
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
