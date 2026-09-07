// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// The Diagnostic Console: PLAN.md section 27.
//
// A request typed or picked, the answer beside it, and how long the ECU took.
// That last number is the one an engineer reads first and the one most tools
// bury: an ECU answering ReadDataByIdentifier in 12 ms and the same ECU
// answering it in 900 ms are two different ECUs, and nothing else on the screen
// says so.
//
// The panel drives a DiagnosticSession, which is the executor's end of the
// conversation - see that header for why it is a mutex the executor never waits
// on. Everything here happens on the GUI thread and nothing here blocks: Send
// queues a request and returns, and the answers arrive on the same 20 Hz timer
// every other panel uses.
//
// Two things it deliberately does not do:
//
//   * **It does not own the addresses.** The request and response identifiers
//     belong to the UDS block on the canvas, which is where a project stores
//     them. Showing them here and letting them be edited in two places is how
//     they end up disagreeing.
//
//   * **It does not decode a response beyond naming its service.** The VIN in a
//     0x62 F1 90 answer is ASCII and shown as ASCII; anything more needs an ODX
//     or a DID database, which is v0.13's problem. Guessing at a layout would
//     put wrong numbers on a screen people make decisions from.

#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include <cstdint>
#include <vector>

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace torquebus {
class DiagnosticSession;
struct UdsExchange;
}

namespace torquebus::ui {

class DiagnosticsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit DiagnosticsPanel(QWidget* parent = nullptr);

    /// Attaches the conversation this panel drives. Not owned; must outlive the
    /// panel. Null detaches it.
    void setSession(DiagnosticSession* session);

    /// Names the ECU being talked to - the block's identifiers, as text.
    /// Empty when no UDS block is in the running graph.
    void setTarget(const QString& description);

private Q_SLOTS:
    /// Drains the session and repaints. The panel's whole clock.
    void refresh();

    void onSend();
    void onRequestChanged(const QString& text);

    /// A different service was picked: rebuild the form under it.
    void onServiceChanged(int index);

    /// A field was edited: reassemble the bytes.
    void onFieldChanged();

    void onClear();
    void onThemeChanged();

private:
    void buildUi();
    void updateAvailability();

    /// Replaces the field editors with the ones the chosen service takes.
    void buildForm();

    /// Reads the editors, assembles the request and writes it into the hex box.
    ///
    /// The hex box is the one thing that gets sent, so the form is a way of
    /// writing into it rather than a second path to the bus: what somebody sees
    /// is what goes out, and a form that produced bytes nobody could look at
    /// would be a worse tool for learning the protocol than the box alone.
    void assembleFromForm();

    /// Adds one exchange to the log: the request, the answer, and the time.
    void appendExchange(const UdsExchange& exchange);

    /// Adds a row, scrolling to it only when the view was already at the
    /// bottom - so reading something twenty rows up is not interrupted by an
    /// answer arriving.
    void appendRow(const QString& time,
                   const QString& direction,
                   const QString& bytes,
                   const QString& meaning,
                   const QColor& tint);

    DiagnosticSession* m_session{nullptr};

    QLabel* m_targetLabel{nullptr};
    QLabel* m_sessionLabel{nullptr};

    /// What is wrong with the form, when something is. Empty otherwise.
    QLabel* m_hintLabel{nullptr};

    QComboBox* m_service{nullptr};
    QFormLayout* m_form{nullptr};

    /// One editor per field of the chosen service, in field order.
    QList<QWidget*> m_fields;

    QLineEdit* m_request{nullptr};
    QPushButton* m_send{nullptr};
    QPushButton* m_clear{nullptr};

    QTableWidget* m_log{nullptr};

    /// Whether the transport was last drawn as available, so the controls are
    /// only re-enabled when that actually changed.
    bool m_shownAsActive{false};

};

} // namespace torquebus::ui
