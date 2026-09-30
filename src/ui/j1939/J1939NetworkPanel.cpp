// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/j1939/J1939NetworkPanel.h"

#include "core/j1939/J1939NameTables.h"
#include "core/j1939/J1939Network.h"
#include "ui/theme/ThemeManager.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QShowEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <optional>
#include <string_view>

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
    return QStringLiteral("%1  (0x%2)")
        .arg(address)
        .arg(address, 2, 16, QLatin1Char('0'))
        .toUpper();
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

[[nodiscard]] QString faultText(const J1939Dtc& fault)
{
    const QString suffix =
        QObject::tr("FMI %1, seen %2x").arg(fault.fmi).arg(fault.occurrenceCount);

    if (fault.spnAssembled) {
        return QObject::tr("SPN %1, %2").arg(fault.spn).arg(suffix);
    }

    // No SPN, and the bytes instead - because a number read under the wrong
    // packing still looks like an SPN, and somebody who knows this ECU reads
    // the right convention out of the bytes in a second.
    return QObject::tr("SPN not assembled (bytes %1 %2 %3 %4), %5")
        .arg(fault.raw[0], 2, 16, QLatin1Char('0'))
        .arg(fault.raw[1], 2, 16, QLatin1Char('0'))
        .arg(fault.raw[2], 2, 16, QLatin1Char('0'))
        .arg(fault.raw[3], 2, 16, QLatin1Char('0'))
        .arg(suffix)
        .toUpper();
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
    header->addWidget(m_summary, 1);

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
           "that never claimed its address."));

    m_nodes->header()->setSectionResizeMode(ColumnAddress, QHeaderView::ResizeToContents);
    m_nodes->header()->setSectionResizeMode(ColumnName, QHeaderView::Interactive);
    m_nodes->header()->setSectionResizeMode(ColumnState, QHeaderView::ResizeToContents);
    m_nodes->header()->setSectionResizeMode(ColumnFrames, QHeaderView::ResizeToContents);
    m_nodes->header()->setSectionResizeMode(ColumnDetail, QHeaderView::Stretch);
    m_nodes->setColumnWidth(ColumnName, 260);

    layout->addWidget(m_nodes, 1);
}

void J1939NetworkPanel::setNameTables(const J1939NameTables* tables)
{
    m_names = tables;
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
    m_sawPublication = true;

    rebuild(snapshot);
    updateSummary(snapshot);
}

void J1939NetworkPanel::rebuild(const J1939NetworkSnapshot& snapshot)
{
    const Theme theme = currentTheme();

    m_nodes->setUpdatesEnabled(false);
    m_nodes->clear();

    for (const J1939NetworkNode& node : snapshot.nodes) {
        auto* row = new QTreeWidgetItem(m_nodes);
        row->setText(ColumnAddress, addressText(node.address));
        row->setText(ColumnFrames, QString::number(node.framesSeen));

        row->setText(ColumnDetail,
                     tr("first seen %1, last %2")
                         .arg(elapsedText(node.firstSeenNs), elapsedText(node.lastSeenNs)));

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

        // What is wrong with this ECU, under it.
        for (const J1939Diagnostic& message : snapshot.diagnostics) {
            if (message.sourceAddress != node.address) {
                continue;
            }

            auto* group = new QTreeWidgetItem(row);
            group->setText(ColumnAddress,
                           message.active ? tr("active faults") : tr("previously active"));
            group->setText(ColumnName,
                           tr("MIL %1, red stop %2, amber %3, protect %4")
                               .arg(lampText(message.lamps.malfunction),
                                    lampText(message.lamps.redStop),
                                    lampText(message.lamps.amberWarning),
                                    lampText(message.lamps.protect)));

            if (message.faults.empty()) {
                // A statement, not a blank. An ECU that says it is fine is a
                // different thing from one that has not answered.
                group->setText(ColumnState, tr("none"));
                group->setForeground(ColumnState, theme.success);
                continue;
            }

            group->setText(ColumnState, QString::number(message.faults.size()));
            group->setForeground(ColumnState, message.active ? theme.error : theme.warning);

            for (const J1939Dtc& fault : message.faults) {
                auto* item = new QTreeWidgetItem(group);
                item->setText(ColumnDetail, faultText(fault));
            }

            // Faults open themselves; a healthy list stays folded.
            group->setExpanded(message.active);
        }
    }

    // The ECUs that could not get an address at all, kept apart because 254 is
    // not a seat and listing them among the occupants would invent one.
    for (const J1939Defeated& entry : snapshot.defeated) {
        auto* row = new QTreeWidgetItem(m_nodes);
        row->setText(ColumnAddress, tr("none"));
        row->setText(ColumnName, nameText(entry.name, m_names));
        row->setText(ColumnState, tr("no address"));
        row->setForeground(ColumnState, theme.warning);
        row->setText(ColumnDetail,
                     tr("announced it could not claim one, %1x - it is on the bus and "
                        "cannot be addressed")
                         .arg(entry.announcements));
    }

    m_nodes->setUpdatesEnabled(true);
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

    std::size_t withFaults = 0;
    std::size_t faults = 0;
    std::size_t unclaimed = 0;

    for (const J1939Diagnostic& message : snapshot.diagnostics) {
        if (message.active && !message.faults.empty()) {
            ++withFaults;
            faults += message.faults.size();
        }
    }

    for (const J1939NetworkNode& node : snapshot.nodes) {
        if (!node.claimSeen) {
            ++unclaimed;
        }
    }

    QString text = tr("%n ECU(s)", nullptr, static_cast<int>(snapshot.nodes.size()));

    if (unclaimed > 0) {
        text += tr(", %n not seen claiming", nullptr, static_cast<int>(unclaimed));
    }

    if (!snapshot.defeated.empty()) {
        text += tr(", %n with no address", nullptr, static_cast<int>(snapshot.defeated.size()));
    }

    text += faults > 0 ? tr(" - %1 active fault(s) on %2").arg(faults).arg(withFaults)
                       : tr(" - no active faults");

    m_summary->setText(text);
    m_summary->setStyleSheet(
        QStringLiteral("color: %1;").arg((faults > 0 ? theme.error : theme.text).name()));
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
