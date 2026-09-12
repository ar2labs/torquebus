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
// --- The three things this panel must not imply ------------------------------
//
//   * **An empty table before Start is not an empty bus.** Nothing published
//     and nobody transmitting look identical, so the panel says which one it is
//     showing rather than leaving somebody to guess.
//   * **"Not seen claiming" is not "never claimed".** Claims happen at power-up;
//     a measurement started on a running machine sees none of them. The column
//     says what was observed since Start, and the tooltip says so in words.
//   * **An ECU reporting no faults is not an ECU that has not answered.** The
//     first is a diagnosis and the second is a question still open.
//
// It reads a J1939Network the block fills - see that header for why the
// hand-over is a snapshot rather than a lock, and why this panel asks for a
// revision number before it asks for anything else.

#pragma once

#include <QWidget>

#include <cstdint>

class QLabel;
class QTreeWidget;

namespace torquebus {
class J1939Network;
struct J1939NetworkSnapshot;
}

namespace torquebus::ui {

class J1939NetworkPanel final : public QWidget {
    Q_OBJECT

public:
    explicit J1939NetworkPanel(QWidget* parent = nullptr);

    /// The bus this panel shows. Not owned; must outlive the panel.
    void setNetwork(J1939Network* network);

private Q_SLOTS:
    /// Asks the revision counter whether anything moved, and rebuilds only when
    /// it did. The panel's whole clock.
    void refresh();

    void onThemeChanged();

private:
    void buildUi();

    void rebuild(const J1939NetworkSnapshot& snapshot);

    void updateSummary(const J1939NetworkSnapshot& snapshot);

    J1939Network* m_network{nullptr};

    QLabel* m_summary{nullptr};
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
