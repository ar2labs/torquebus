// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/j1939/J1939NetworkPanel.h"

#include "core/j1939/J1939Id.h"
#include "core/j1939/J1939NameTables.h"
#include "core/j1939/J1939Network.h"
#include "ui/j1939/J1939RequestDialog.h"
#include "ui/theme/ThemeManager.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace torquebus::ui {
namespace {

/// 20 Hz, like every other panel that reads shared state on a timer.
constexpr int kRefreshMs = 50;

enum Column : int {
    ColumnAddress = 0,
    ColumnName,
    ColumnState,
    ColumnFrames,
    ColumnDetail,
    ColumnCount,
};

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }

    return Theme::dark();
}

/// An address as people write them: decimal and hex together, because a
/// database says 249 and a trace says 0xF9 and they are the same ECU.
[[nodiscard]] QString addressText(std::uint8_t address)
{
    // The digits are upper-cased and the "0x" in front of them is not.
    return QStringLiteral("%1  (0x%2)")
        .arg(address)
        .arg(QString::number(address, 16).toUpper(), 2, QLatin1Char('0'));
}

/// The industry group by name. One of the few J1939 tables small enough and
/// fixed enough to live in code - see J1939Name.h for why the others do not.
[[nodiscard]] QString industryGroupText(std::uint8_t industryGroup)
{
    const std::string_view name = j1939IndustryGroupName(industryGroup);

    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

[[nodiscard]] QString elapsedText(std::uint64_t nanoseconds)
{
    return QStringLiteral("%1 s").arg(
        static_cast<double>(nanoseconds) / 1'000'000'000.0, 0, 'f', 3);
}

/// A number, or the word for it when this machine has one.
///
/// The number stays either way. A row that showed only "Transmission" would be
/// unusable the moment somebody needs to compare it against a database, and the
/// number is what they would then go looking for.
[[nodiscard]] QString named(std::optional<std::string_view> word, std::uint32_t number)
{
    if (!word.has_value()) {
        return QString::number(number);
    }

    return QStringLiteral("%1 (%2)")
        .arg(QString::fromUtf8(word->data(), static_cast<qsizetype>(word->size())))
        .arg(number);
}

/// The identity of an ECU in one line.
///
/// Manufacturer and serial are what make a NAME unique, and the function is
/// what somebody is usually scanning for - so all three, with words wherever
/// this machine has a table to supply them.
[[nodiscard]] QString nameText(const J1939Name& name, const J1939NameTables* tables)
{
    const auto lookup = [tables](auto&& getter) -> std::optional<std::string_view> {
        return tables != nullptr ? getter(*tables) : std::nullopt;
    };

    const QString function = named(
        lookup([&name](const J1939NameTables& t) { return t.function(name); }), name.function);

    const QString manufacturer = named(
        lookup([&name](const J1939NameTables& t) { return t.manufacturer(name.manufacturerCode); }),
        name.manufacturerCode);

    return QObject::tr("%1, mfr %2, serial %3")
        .arg(function, manufacturer)
        .arg(name.identityNumber);
}

/// One lamp, in the four states J1939 actually has.
[[nodiscard]] QString lampText(J1939LampState state)
{
    switch (state) {
    case J1939LampState::Off:
        return QObject::tr("off");
    case J1939LampState::On:
        return QObject::tr("ON");
    case J1939LampState::Reserved:
        return QObject::tr("reserved");
    case J1939LampState::NotAvailable:
        break;
    }

    // Said rather than left blank: an ECU that does not drive a lamp is making
    // a statement, and a blank cell reads as a decode that failed.
    return QObject::tr("not fitted");
}

/// Whether the ECU at `address` on `bus` is still repeating its DM1, which is what makes the last
/// one it sent what it is saying now. The ones with nothing to report count: a healthy ECU is on
/// the list the block hands over, and has no code on the list of faults to be found by.
[[nodiscard]] bool isReportingNow(const J1939BusView& bus, std::uint8_t address)
{
    return std::find(bus.reporting.begin(), bus.reporting.end(), address) != bus.reporting.end();
}

/// A bus as a person names it: "CAN 2". Counted from one, as the channels are everywhere else.
[[nodiscard]] QString busText(std::uint8_t channel)
{
    return QObject::tr("CAN %1").arg(static_cast<int>(channel) + 1);
}

/// What a row is remembered by from one rebuild to the next - its bus, and for an ECU its address
/// on that bus - and whether it is the heading of a bus.
constexpr int kRowKeyRole = Qt::UserRole;
constexpr int kBusHeadingRole = Qt::UserRole + 1;

/// What a row can be asked: the channel of its bus and the address it stands for - 255 for the
/// heading of a bus, which is every ECU on it.
constexpr int kChannelRole = Qt::UserRole + 2;
constexpr int kAddressRole = Qt::UserRole + 3;
constexpr int kEveryEcu = 255;

/// Whom a request is addressed to, in a sentence.
[[nodiscard]] QString whomText(int address)
{
    return address == kEveryEcu
               ? QObject::tr("every ECU")
               : QObject::tr("the ECU at %1").arg(addressText(static_cast<std::uint8_t>(address)));
}

/// What an ECU last said to a request it could not meet, in a few words.
[[nodiscard]] QString answerText(const J1939Acknowledgement& answer)
{
    QString what;

    switch (answer.control) {
    case kJ1939AckPositive:
        what = QObject::tr("acknowledged");
        break;
    case kJ1939AckNegative:
        what = QObject::tr("not supported");
        break;
    case kJ1939AckAccessDenied:
        what = QObject::tr("access denied");
        break;
    case kJ1939AckCannotRespond:
        what = QObject::tr("cannot respond");
        break;
    default:
        what = QObject::tr("answer %1").arg(answer.control);
        break;
    }

    return QObject::tr("last answer: PGN %1 %2").arg(J1939RequestDialog::pgnText(answer.pgn), what);
}

[[nodiscard]] QString busKey(std::uint8_t channel)
{
    return QStringLiteral("bus %1").arg(channel);
}

[[nodiscard]] QString ecuKey(std::uint8_t channel, std::uint8_t address)
{
    return QStringLiteral("%1/%2").arg(channel).arg(address);
}

[[nodiscard]] QString faultText(const J1939Dtc& fault)
{
    const std::string_view fmiDescription = j1939FmiName(fault.fmi);
    const QString fmiWord =
        QString::fromUtf8(fmiDescription.data(), static_cast<qsizetype>(fmiDescription.size()));
    const QString suffix =
        QObject::tr("FMI %1 (%2), seen %3x").arg(fault.fmi).arg(fmiWord).arg(fault.occurrenceCount);

    if (fault.spnAssembled) {
        return QObject::tr("SPN %1, %2").arg(fault.spn).arg(suffix);
    }

    // No SPN, and the bytes instead - because a number read under the wrong
    // packing still looks like an SPN, and somebody who knows this ECU reads
    // the right convention out of the bytes in a second.
    const QString rawBytes = QStringLiteral("%1 %2 %3 %4")
                                 .arg(fault.raw[0], 2, 16, QLatin1Char('0'))
                                 .arg(fault.raw[1], 2, 16, QLatin1Char('0'))
                                 .arg(fault.raw[2], 2, 16, QLatin1Char('0'))
                                 .arg(fault.raw[3], 2, 16, QLatin1Char('0'))
                                 .toUpper();
    return QObject::tr("SPN not assembled (bytes %1), %2").arg(rawBytes, suffix);
}

} // namespace

J1939NetworkPanel::J1939NetworkPanel(QWidget* parent)
    : QWidget{parent}
{
    buildUi();

    auto* timer = new QTimer(this);
    timer->setInterval(kRefreshMs);
    timer->setTimerType(Qt::CoarseTimer);
    connect(timer, &QTimer::timeout, this, &J1939NetworkPanel::refresh);
    timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }

    updateSummary(J1939NetworkSnapshot{});
}

void J1939NetworkPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void J1939NetworkPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);

    m_summary = new QLabel;
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // It may wrap: with two buttons beside it, a line that cannot would keep the panel as wide as
    // the whole of it, in a dock that is meant to be a column.
    m_summary->setWordWrap(true);
    header->addWidget(m_summary, 1);

    m_requestClaims = new QPushButton(tr("Request claims"));
    m_requestClaims->setObjectName(QStringLiteral("requestClaimsButton"));
    connect(m_requestClaims, &QPushButton::clicked, this, &J1939NetworkPanel::claimsRequested);
    header->addWidget(m_requestClaims);

    m_requestGroup = new QPushButton(tr("Request PGN..."));
    m_requestGroup->setObjectName(QStringLiteral("requestPgnButton"));
    connect(m_requestGroup, &QPushButton::clicked, this, [this] { askForAGroup(-1, kEveryEcu); });
    header->addWidget(m_requestGroup);

    setRequestAvailable(false);

    layout->addLayout(header);

    m_nodes = new QTreeWidget;
    m_nodes->setColumnCount(ColumnCount);
    m_nodes->setHeaderLabels({tr("Address"), tr("NAME"), tr("State"), tr("Frames"), tr("Detail")});
    m_nodes->setRootIsDecorated(true);
    m_nodes->setUniformRowHeights(true);
    m_nodes->setAlternatingRowColors(true);
    m_nodes->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nodes->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_nodes->headerItem()->setToolTip(
        ColumnState,
        tr("What was observed since this measurement started. Address claims happen "
           "at power-up, so a machine that was already running when you connected "
           "shows every ECU as not seen claiming - which is not the same as an ECU "
           "that never claimed its address. Request claims asks them to claim again."));

    m_nodes->header()->setSectionResizeMode(ColumnAddress, QHeaderView::ResizeToContents);
    m_nodes->header()->setSectionResizeMode(ColumnName, QHeaderView::Interactive);
    m_nodes->header()->setSectionResizeMode(ColumnState, QHeaderView::ResizeToContents);
    m_nodes->header()->setSectionResizeMode(ColumnFrames, QHeaderView::ResizeToContents);
    m_nodes->header()->setSectionResizeMode(ColumnDetail, QHeaderView::Stretch);
    m_nodes->setColumnWidth(ColumnName, 260);

    // A right click on an ECU asks it for something; on a bus, every ECU on it.
    m_nodes->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        m_nodes, &QTreeWidget::customContextMenuRequested, this, &J1939NetworkPanel::onContextMenu);

    layout->addWidget(m_nodes, 1);
}

void J1939NetworkPanel::setNameTables(const J1939NameTables* tables)
{
    m_names = tables;
}

void J1939NetworkPanel::setRequestAvailable(bool available)
{
    m_requestAvailable = available;

    m_requestClaims->setEnabled(available);
    m_requestGroup->setEnabled(available);

    const QString claims = tr("Asks every ECU on every channel to announce its address and NAME "
                              "again, with a Request for Address Claimed sent from the null "
                              "address. Use it when the measurement started on a machine that was "
                              "already running: claims are made at power-up, so the ECUs that have "
                              "not claimed since Start are listed as not seen claiming.");

    const QString group =
        tr("Asks one ECU, or every ECU, for a parameter group: the codes that were "
           "active (DM2), what an ECU and its software are called, or any other "
           "PGN. The same is in the menu of a right click on an ECU. It is sent "
           "from the null address, 254, as the tool has none of its own. What an "
           "ECU does not have, it says so: its last answer is on its row.");

    const QString unavailable = tr(" Available while a measurement is running.");

    m_requestClaims->setToolTip(available ? claims : claims + unavailable);
    m_requestGroup->setToolTip(available ? group : group + unavailable);
}

void J1939NetworkPanel::setNetwork(J1939Network* network)
{
    m_network = network;

    m_nodes->clear();
    m_shownRevision = 0U;
    m_sawPublication = false;

    updateSummary(J1939NetworkSnapshot{});
}

void J1939NetworkPanel::refresh()
{
    if (m_network == nullptr || !isVisible()) {
        return;
    }

    // The cheap question first. Copying the table to find out whether it
    // changed would be doing the expensive half of the work for nothing.
    const std::uint64_t revision = m_network->revision();
    if (revision == m_shownRevision) {
        return;
    }

    const J1939NetworkSnapshot snapshot = m_network->snapshot();
    m_shownRevision = snapshot.revision;
    m_sawPublication = snapshot.published;

    rebuild(snapshot);
    updateSummary(snapshot);
}

void J1939NetworkPanel::rebuild(const J1939NetworkSnapshot& snapshot)
{
    const Theme theme = currentTheme();

    // What was open stays open and what was closed stays closed: an ECU row is remembered by its
    // bus and its address, and the heading of a bus by its bus.
    QSet<QString> expandedRows;
    QSet<QString> collapsedBuses;

    for (int i = 0; i < m_nodes->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* top = m_nodes->topLevelItem(i);

        if (!top->data(ColumnAddress, kBusHeadingRole).toBool()) {
            if (top->isExpanded()) {
                expandedRows.insert(top->data(ColumnAddress, kRowKeyRole).toString());
            }
            continue;
        }

        if (!top->isExpanded()) {
            collapsedBuses.insert(top->data(ColumnAddress, kRowKeyRole).toString());
        }

        for (int child = 0; child < top->childCount(); ++child) {
            if (const QTreeWidgetItem* ecu = top->child(child); ecu->isExpanded()) {
                expandedRows.insert(ecu->data(ColumnAddress, kRowKeyRole).toString());
            }
        }
    }

    m_nodes->setUpdatesEnabled(false);
    m_nodes->clear();

    const bool severalBuses = snapshot.buses.size() > 1U;

    for (const J1939BusView& bus : snapshot.buses) {
        QTreeWidgetItem* heading = nullptr;

        if (severalBuses) {
            // An address is a seat on one bus, so with more than one the ECUs are listed under the
            // bus they are on: the same number on another bus is another ECU.
            heading = new QTreeWidgetItem(m_nodes);
            heading->setData(ColumnAddress, kRowKeyRole, busKey(bus.channel));
            heading->setData(ColumnAddress, kBusHeadingRole, true);
            heading->setData(ColumnAddress, kChannelRole, static_cast<int>(bus.channel));
            heading->setData(ColumnAddress, kAddressRole, kEveryEcu);
            heading->setText(ColumnAddress, busText(bus.channel));
            heading->setText(ColumnName,
                             tr("%n ECU(s)", nullptr, static_cast<int>(bus.nodes.size())));
            heading->setText(
                ColumnState,
                bus.activeFaults.empty()
                    ? tr("no active faults")
                    : tr("%n active fault(s)", nullptr, static_cast<int>(bus.activeFaults.size())));
            heading->setForeground(ColumnState,
                                   bus.activeFaults.empty() ? theme.textMuted : theme.error);
        }

        addBus(heading, bus, expandedRows);

        if (heading != nullptr) {
            // Open unless somebody closed it: a heading that is closed hides what the panel is for.
            heading->setExpanded(!collapsedBuses.contains(busKey(bus.channel)));
        }
    }

    m_nodes->setUpdatesEnabled(true);
}

void J1939NetworkPanel::addBus(QTreeWidgetItem* heading,
                               const J1939BusView& bus,
                               const QSet<QString>& expandedRows)
{
    const Theme theme = currentTheme();

    // Under the heading of the bus when there is one, and at the top of the tree when the
    // measurement has one bus - which is the usual case, and is shown as it always was.
    const auto newRow = [this, heading] {
        return heading != nullptr ? new QTreeWidgetItem(heading) : new QTreeWidgetItem(m_nodes);
    };

    for (const J1939NetworkNode& node : bus.nodes) {
        QTreeWidgetItem* row = newRow();
        const QString key = ecuKey(bus.channel, node.address);
        row->setData(ColumnAddress, kRowKeyRole, key);
        row->setData(ColumnAddress, kChannelRole, static_cast<int>(bus.channel));
        row->setData(ColumnAddress, kAddressRole, static_cast<int>(node.address));
        row->setText(ColumnAddress, addressText(node.address));
        row->setText(ColumnFrames, QString::number(node.framesSeen));

        QString detail = tr("first seen %1, last %2")
                             .arg(elapsedText(node.firstSeenNs), elapsedText(node.lastSeenNs));

        // What it said to a request it could not meet, when it has: it is not a code and it is not
        // a fault, and it would be nowhere else on the screen.
        if (node.lastAnswer.has_value()) {
            detail += QStringLiteral("; ") + answerText(*node.lastAnswer);
        }

        row->setText(ColumnDetail, detail);

        if (node.name.has_value()) {
            row->setText(ColumnName, nameText(*node.name, m_names));
            row->setText(ColumnState, tr("claimed"));

            // The industry group decides what the vehicle system and a function
            // above 127 mean, so it belongs with the NAME rather than in a
            // column of its own that would be the same word on every row.
            row->setToolTip(
                ColumnName,
                tr("Industry group: %1").arg(industryGroupText(node.name->industryGroup)));
        } else {
            // Not "unknown": the ECU is there and transmitting, and the only
            // thing missing is a claim this measurement was around to hear.
            row->setText(ColumnName, tr("no claim seen"));
            row->setText(ColumnState, tr("transmitting"));
            row->setForeground(ColumnName, theme.textMuted);
        }

        bool hasActiveFaults = false;

        // What is wrong with this ECU, under it.
        for (const J1939Diagnostic& message : bus.diagnostics) {
            if (message.sourceAddress != node.address) {
                continue;
            }

            // A DM1 is what an ECU is saying only while it goes on saying it. One whose ECU is not
            // repeating it is the last thing that ECU said: it has gone quiet, or the measurement
            // has stopped. The faults it lists are still worth reading - that is why the panel
            // keeps them - and are not what is wrong with the machine now. Nor is "none" a
            // diagnosis any more: an ECU that said it had no faults, and has not said so since, is
            // not saying it.
            const bool lastReported = message.active && !isReportingNow(bus, node.address);

            auto* group = new QTreeWidgetItem(row);
            group->setText(ColumnAddress,
                           !message.active
                               ? tr("previously active")
                               : (lastReported ? tr("last reported") : tr("active faults")));

            if (lastReported) {
                const QString whatItSaid =
                    message.faults.empty()
                        ? tr("It said it had no faults, which is not something it is saying now.")
                        : tr("These faults are not being reported now.");

                group->setToolTip(
                    ColumnAddress,
                    tr("The last DM1 this ECU sent, and it has not sent another: it has gone "
                       "quiet, or the measurement has stopped. %1")
                        .arg(whatItSaid));
            }

            group->setText(ColumnName,
                           tr("MIL %1, red stop %2, amber %3, protect %4")
                               .arg(lampText(message.lamps.malfunction),
                                    lampText(message.lamps.redStop),
                                    lampText(message.lamps.amberWarning),
                                    lampText(message.lamps.protect)));

            if (message.faults.empty()) {
                // A statement, not a blank. An ECU that says it is fine is a different thing from
                // one that has not answered - and from one that said so and has gone quiet, whose
                // "none" is kept in the colour of what is not being said now.
                group->setText(ColumnState, tr("none"));
                group->setForeground(ColumnState, lastReported ? theme.textMuted : theme.success);
                continue;
            }

            group->setText(ColumnState, QString::number(message.faults.size()));
            group->setForeground(ColumnState,
                                 message.active && !lastReported ? theme.error : theme.warning);

            for (const J1939Dtc& fault : message.faults) {
                auto* item = new QTreeWidgetItem(group);
                item->setText(ColumnDetail, faultText(fault));
            }

            // Faults open themselves; a healthy list stays folded.
            group->setExpanded(message.active);
            if (message.active) {
                hasActiveFaults = true;
            }
        }

        if (hasActiveFaults || expandedRows.contains(key)) {
            row->setExpanded(true);
        }
    }

    // The ECUs that could not get an address at all, kept apart because 254 is
    // not a seat and listing them among the occupants would invent one.
    for (const J1939Defeated& entry : bus.defeated) {
        QTreeWidgetItem* row = newRow();
        row->setText(ColumnAddress, tr("none"));
        row->setText(ColumnName, nameText(entry.name, m_names));
        row->setText(ColumnState, tr("no address"));
        row->setForeground(ColumnState, theme.warning);
        row->setText(ColumnDetail,
                     tr("announced it could not claim one, %1x - it is on the bus and "
                        "cannot be addressed")
                         .arg(entry.announcements));
    }
}

void J1939NetworkPanel::updateSummary(const J1939NetworkSnapshot& snapshot)
{
    const Theme theme = currentTheme();

    if (!m_sawPublication) {
        // Before Start, and a silent bus, show the same empty table. Saying
        // which one this is costs a sentence and saves somebody looking for a
        // wiring fault that is not there.
        m_summary->setText(m_network == nullptr
                               ? tr("No J1939 block in this graph.")
                               : tr("Nothing seen yet - start a measurement, or check "
                                    "that a J1939 block is wired to a channel."));
        m_summary->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textMuted.name()));
        return;
    }

    // What the buses are reporting now, and not what each ECU last said: after a measurement ends,
    // or for an ECU that has gone quiet, the second is still in the tree and the first is nothing.
    std::size_t ecus = 0;
    std::size_t withFaults = 0;
    std::size_t faults = 0;
    std::size_t unclaimed = 0;
    std::size_t defeated = 0;
    std::size_t reportedEarlier = 0;

    for (const J1939BusView& bus : snapshot.buses) {
        ecus += bus.nodes.size();
        defeated += bus.defeated.size();
        faults += bus.activeFaults.size();

        // In address order, so an ECU's codes are together - and an ECU is an address on its bus.
        for (std::size_t i = 0; i < bus.activeFaults.size(); ++i) {
            if (i == 0
                || bus.activeFaults[i].sourceAddress != bus.activeFaults[i - 1].sourceAddress) {
                ++withFaults;
            }
        }

        std::size_t said = 0;

        for (const J1939Diagnostic& message : bus.diagnostics) {
            if (message.active) {
                said += message.faults.size();
            }
        }

        reportedEarlier += said - std::min(said, bus.activeFaults.size());

        for (const J1939NetworkNode& node : bus.nodes) {
            if (!node.claimSeen) {
                ++unclaimed;
            }
        }
    }

    QString text = tr("%n ECU(s)", nullptr, static_cast<int>(ecus));

    if (snapshot.buses.size() > 1U) {
        text += tr(" on %n buses", nullptr, static_cast<int>(snapshot.buses.size()));
    }

    if (unclaimed > 0) {
        text += tr(", %n not seen claiming", nullptr, static_cast<int>(unclaimed));
    }

    if (defeated > 0) {
        text += tr(", %n with no address", nullptr, static_cast<int>(defeated));
    }

    text += faults > 0 ? tr(" - %1 active fault(s) on %2").arg(faults).arg(withFaults)
                       : tr(" - no active faults");

    if (reportedEarlier > 0) {
        text += tr(", %n reported earlier", nullptr, static_cast<int>(reportedEarlier));
    }

    m_summary->setText(text);
    m_summary->setStyleSheet(
        QStringLiteral("color: %1;").arg((faults > 0 ? theme.error : theme.text).name()));
}

QMenu* J1939NetworkPanel::requestMenu(int channel, int address, QWidget* parent)
{
    auto* menu = new QMenu(parent);

    const QString where =
        channel < 0 ? tr("every bus") : busText(static_cast<std::uint8_t>(channel));

    menu->setTitle(address == kEveryEcu ? tr("Ask every ECU on %1").arg(where)
                                        : tr("Ask %1").arg(whomText(address)));

    // A popup does not show its title, and what follows are groups with no object: "Address claim"
    // asks nobody until a line above it says whom.
    QAction* heading = menu->addAction(menu->title());
    heading->setObjectName(QStringLiteral("requestMenuHeading"));
    heading->setEnabled(false);
    menu->addSeparator();

    for (const J1939RequestPreset& preset : J1939RequestDialog::presets()) {
        // Named, and not numbered: the number is on the dialog's list, and the name is what it is.
        QAction* action = menu->addAction(preset.label);
        action->setEnabled(m_requestAvailable);

        connect(action, &QAction::triggered, this, [this, pgn = preset.pgn, address, channel] {
            Q_EMIT pgnRequested(pgn, address, channel);
        });
    }

    menu->addSeparator();

    QAction* other = menu->addAction(tr("Another group..."));
    other->setEnabled(m_requestAvailable);
    connect(other, &QAction::triggered, this, [this, channel, address] {
        askForAGroup(channel, address);
    });

    return menu;
}

void J1939NetworkPanel::onContextMenu(const QPoint& position)
{
    // The row, or the ECU or bus a row under it belongs to: a fault is asked about by asking its
    // ECU.
    const QTreeWidgetItem* item = m_nodes->itemAt(position);
    while (item != nullptr && !item->data(ColumnAddress, kAddressRole).isValid()) {
        item = item->parent();
    }

    if (item == nullptr) {
        return;
    }

    const int channel = item->data(ColumnAddress, kChannelRole).toInt();
    const int address = item->data(ColumnAddress, kAddressRole).toInt();

    // Read before the menu opens: the tree is rebuilt while it is open, and the row is gone.
    const std::unique_ptr<QMenu> menu{requestMenu(channel, address, this)};
    menu->exec(m_nodes->viewport()->mapToGlobal(position));
}

void J1939NetworkPanel::askForAGroup(int channel, int address)
{
    // Whom and where can be chosen from what the panel has heard.
    std::vector<J1939RequestDialog::Ecu> ecus;
    std::vector<J1939RequestDialog::Bus> buses;

    if (m_network != nullptr) {
        for (const J1939BusView& bus : m_network->snapshot().buses) {
            buses.push_back({static_cast<int>(bus.channel), busText(bus.channel)});

            for (const J1939NetworkNode& node : bus.nodes) {
                const bool listed =
                    std::any_of(ecus.begin(), ecus.end(), [&node](const auto& known) {
                        return known.address == node.address;
                    });

                if (!listed) {
                    ecus.push_back({static_cast<int>(node.address),
                                    tr("ECU at %1").arg(addressText(node.address))});
                }
            }
        }
    }

    J1939RequestDialog dialog{std::move(ecus), std::move(buses), this};

    if (address != kEveryEcu) {
        dialog.setDestination(address);
    }

    if (channel >= 0) {
        dialog.setChannel(channel);
    }

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    if (const std::optional<std::uint32_t> pgn = dialog.pgn(); pgn.has_value()) {
        Q_EMIT pgnRequested(*pgn, dialog.destination(), dialog.channel());
    }
}

void J1939NetworkPanel::onThemeChanged()
{
    if (m_network == nullptr) {
        updateSummary(J1939NetworkSnapshot{});
        return;
    }

    // Colours are baked into the items, so a theme change means building them
    // again rather than repainting what is there.
    const J1939NetworkSnapshot snapshot = m_network->snapshot();
    rebuild(snapshot);
    updateSummary(snapshot);
}

} // namespace torquebus::ui
