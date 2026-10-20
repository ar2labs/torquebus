// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Who is on this bus.
//
// The first question somebody has when they connect to a machine they did not
// build, and the one a trace cannot answer: a trace shows identifiers going
// past, and turning those into "an engine, a gearbox and something nobody can
// name" is the work this panel does.
//
// One row per address, opening into what the NAME says and what is wrong with
// that ECU right now. Sorted by address, because that is the order a person
// scanning for a gap reads in.
//
// --- The five things this panel must not imply --------------------------------
//
//   * **An empty table before Start is not an empty bus.** Nothing published
//     and nobody transmitting look identical, so the panel says which one it is
//     showing rather than leaving somebody to guess.
//   * **"Not seen claiming" is not "never claimed".** Claims happen at power-up;
//     a measurement started on a running machine sees none of them. The column
//     says what was observed since Start, and the tooltip says so in words - and
//     says what to do about it: the Request claims button asks every ECU to claim
//     again (J1939ClaimRequest.h), which is the one thing here that transmits, and
//     only when pressed.
//   * **An ECU reporting no faults is not an ECU that has not answered.** The
//     first is a diagnosis and the second is a question still open.
//   * **What an ECU last said is not what the bus is reporting now.** The tree
//     keeps each ECU's last DM1, which is what a panel is for after a
//     measurement ends; an ECU that has gone quiet, or a measurement that has
//     stopped, leaves that DM1 *last reported* - its faults, or its "none" - and
//     the summary counts only what the block says is being reported now.
//   * **An address is a seat on one bus.** With a J1939 block on more than one
//     bus the ECUs are listed under the bus they are on - "CAN 2" - because the
//     same number on another bus is another ECU. With one bus the tree is what
//     it always was.
//
// It reads a J1939Network the block fills - see that header for why the
// hand-over is a snapshot rather than a lock, and why this panel asks for a
// revision number before it asks for anything else.

#pragma once

#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdint>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace torquebus {
class J1939Network;
class J1939NameTables;
struct J1939BusView;
struct J1939NetworkSnapshot;
} // namespace torquebus

namespace torquebus::ui {

class J1939NetworkPanel final : public QWidget {
    Q_OBJECT

public:
    explicit J1939NetworkPanel(QWidget* parent = nullptr);

    /// The bus this panel shows. Not owned; must outlive the panel.
    void setNetwork(J1939Network* network);

    /// The words behind the numbers in a NAME, or nullptr for numbers only.
    /// Not owned; must outlive the panel. See J1939NameTables.h for why a
    /// machine can legitimately have none.
    void setNameTables(const J1939NameTables* tables);

    /// Whether Request claims can be pressed: while a measurement runs, and not before it or
    /// after it, when every channel would refuse. Off until told.
    void setRequestAvailable(bool available);

Q_SIGNALS:
    /// The Request claims button was pressed. The panel reads a bus and does not own one, so what
    /// to ask on is for whoever owns the engine to say.
    void claimsRequested();

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    /// Asks the revision counter whether anything moved, and rebuilds only when
    /// it did. The panel's whole clock.
    void refresh();

    void onThemeChanged();

private:
    void buildUi();

    void rebuild(const J1939NetworkSnapshot& snapshot);

    /// The ECUs of one bus, under `heading` - or at the top of the tree when it is null, which is
    /// a measurement with one bus. A row whose key is in `expandedRows` is opened again.
    void
    addBus(QTreeWidgetItem* heading, const J1939BusView& bus, const QSet<QString>& expandedRows);

    void updateSummary(const J1939NetworkSnapshot& snapshot);

    J1939Network* m_network{nullptr};
    const J1939NameTables* m_names{nullptr};

    QLabel* m_summary{nullptr};
    QPushButton* m_requestClaims{nullptr};
    QTreeWidget* m_nodes{nullptr};

    /// The revision already on screen. Zero means nothing has been shown, which
    /// is also what the network reports before a measurement has published
    /// anything - so the first publication always repaints.
    std::uint64_t m_shownRevision{0U};

    /// True once anything has been published, so "before Start" and "a silent
    /// bus" can be told apart in words.
    bool m_sawPublication{false};
};

} // namespace torquebus::ui
