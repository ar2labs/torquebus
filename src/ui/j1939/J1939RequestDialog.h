// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Asking for a parameter group: which, of whom, and on which bus.
//
// The J1939 Network panel can ask every ECU for its claim with one button. This is the rest of what
// a service tool asks for - the codes that were active (DM2), what an ECU, its software and its
// components are called, the vehicle's number - and anything else with a PGN, of one ECU or of
// everyone, on one bus or on all of them (J1939Request.h says what comes of it).
//
// A group is named by its number, in decimal (65227) or in hex with the 0x it needs to be read as
// hex (0xFECB), and the list above the field is the ones worth asking for by name. What is typed is
// checked before it can be asked for, in words: a number that is not a PGN, one with more bits than
// a PGN has, and one for a single ECU whose last byte - which is the ECU's address on the wire, and
// not part of the group - is not zero.
//
// Two of the groups a person might type are not questions. Asking an ECU for DM3 or DM11 asks it to
// forget its fault codes, and a tool that sent that without saying what it did would be a tool that
// erased evidence. They can be asked for - it is the person's vehicle - after the dialog says what
// they do.

#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace torquebus::ui {

/// A parameter group worth asking for by name.
struct J1939RequestPreset final {
    std::uint32_t pgn{0};
    QString label;
};

class J1939RequestDialog final : public QDialog {
    Q_OBJECT

public:
    /// One ECU the panel knows of, as the destination list shows it.
    struct Ecu final {
        int address{0};
        QString label;
    };

    /// One bus the panel has heard, as the channel list shows it.
    struct Bus final {
        int channel{0};
        QString label;
    };

    /// "Every ECU" and "every channel" are always there; `ecus` and `buses` are what else can be
    /// chosen. Modal and short-lived.
    J1939RequestDialog(std::vector<Ecu> ecus, std::vector<Bus> buses, QWidget* parent = nullptr);

    /// The groups worth asking for by name.
    [[nodiscard]] static const std::vector<J1939RequestPreset>& presets();

    /// What a typed group is: "65227" or "0xFECB". Nothing for text that is not a number.
    [[nodiscard]] static std::optional<std::uint32_t> parsePgn(const QString& text);

    /// A group as people write it, decimal and hex together - "65227 (0xFECB)" - because a database
    /// says the first and a trace says the second. The same words in the panel and in the Output.
    [[nodiscard]] static QString pgnText(std::uint32_t pgn);

    /// Why the group typed cannot be asked for, in words, or nothing when it can.
    [[nodiscard]] static QString problemWith(const QString& text);

    /// Whether asking for `pgn` asks an ECU to clear its fault codes: DM3 and DM11.
    [[nodiscard]] static bool clearsFaultCodes(std::uint32_t pgn) noexcept;

    // --- What was chosen ----------------------------------------------------------------------

    /// The group, or nothing while what is typed is not one that can be asked for.
    [[nodiscard]] std::optional<std::uint32_t> pgn() const;

    /// The address of the ECU to ask, or 255 for every ECU.
    [[nodiscard]] int destination() const;

    /// The channel to ask on, or -1 for every channel.
    [[nodiscard]] int channel() const;

    // --- For the panel that opens it, and for a test --------------------------------------------

    void setPgnText(const QString& text);
    void setDestination(int address);
    void setChannel(int channel);

    /// Whether Ask can be pressed: the group typed can be asked for.
    [[nodiscard]] bool canAsk() const;

    /// What the line under the field says.
    [[nodiscard]] QString problem() const;

public Q_SLOTS:
    /// Asks for confirmation first when the group clears fault codes.
    void accept() override;

private:
    void buildUi(const std::vector<Ecu>& ecus, const std::vector<Bus>& buses);
    void onPresetChosen(int index);
    void onTextChanged();

    QComboBox* m_group{nullptr};
    QLineEdit* m_pgn{nullptr};
    QLabel* m_problem{nullptr};
    QComboBox* m_destination{nullptr};
    QComboBox* m_channel{nullptr};
    QPushButton* m_ask{nullptr};
};

} // namespace torquebus::ui
