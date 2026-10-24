// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/j1939/J1939RequestDialog.h"

#include "core/j1939/J1939Id.h"
#include "ui/theme/ThemeManager.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace torquebus::ui {
namespace {

// The two groups that are not questions.
constexpr std::uint32_t kPgnDm3 = 0x0'FECCU; // clears the codes that were active
constexpr std::uint32_t kPgnDm11 = 0x0'FED3U; // clears the codes that are

/// What the group at the end of the list says: the one that is whatever is typed.
constexpr int kAnotherGroup = -1;

constexpr int kEveryEcu = 255;
constexpr int kEveryChannel = -1;

/// Whether `character` is a digit of a number written in `base`, which is 10 or 16: the plain ones,
/// and not the digits of other alphabets that Qt's own conversion would refuse.
[[nodiscard]] bool isDigitOf(QChar character, int base)
{
    const char16_t unit = character.unicode();

    if (unit >= u'0' && unit <= u'9') {
        return true;
    }

    return base == 16 && ((unit >= u'a' && unit <= u'f') || (unit >= u'A' && unit <= u'F'));
}

/// The digits of `value` in hex, upper case, and at least `width` of them: the "0x" they follow is
/// not upper-cased with them.
[[nodiscard]] QString hexDigits(std::uint32_t value, int width)
{
    return QString::number(value, 16).toUpper().rightJustified(width, QLatin1Char('0'));
}

} // namespace

const std::vector<J1939RequestPreset>& J1939RequestDialog::presets()
{
    static const std::vector<J1939RequestPreset> kPresets{
        {kPgnAddressClaimed, QObject::tr("Address claim")},
        {kPgnDm1, QObject::tr("Active faults (DM1)")},
        {kPgnDm2, QObject::tr("Previously active faults (DM2)")},
        {0x0'FDC5U, QObject::tr("ECU identification")},
        {0x0'FEDAU, QObject::tr("Software identification")},
        {0x0'FEEBU, QObject::tr("Component identification")},
        {0x0'FEECU, QObject::tr("Vehicle identification")},
    };

    return kPresets;
}

std::optional<std::uint32_t> J1939RequestDialog::parsePgn(const QString& text)
{
    QString digits = text.trimmed();
    int base = 10;

    if (digits.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        digits = digits.mid(2);
        base = 16;
    }

    // Digits and nothing else. toULongLong() would also take a sign, a second "0x" and a number in
    // an alphabet of its own, and none of them is a PGN; and a "0" in front of a decimal number is
    // a number and not a mark that it is octal.
    const auto isDigit = [base](QChar character) { return isDigitOf(character, base); };

    if (digits.isEmpty() || !std::all_of(digits.cbegin(), digits.cend(), isDigit)) {
        return std::nullopt;
    }

    bool ok = false;
    const qulonglong value = digits.toULongLong(&ok, base);

    if (!ok || value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }

    return static_cast<std::uint32_t>(value);
}

QString J1939RequestDialog::pgnText(std::uint32_t pgn)
{
    return QStringLiteral("%1 (0x%2)").arg(pgn).arg(hexDigits(pgn, 4));
}

QString J1939RequestDialog::problemWith(const QString& text)
{
    if (text.trimmed().isEmpty()) {
        return tr("Type the PGN: 65227, or 0xFECB.");
    }

    const std::optional<std::uint32_t> pgn = parsePgn(text);
    if (!pgn.has_value()) {
        return tr("That is not a PGN. Write it in decimal - 65227 - or in hex with 0x - 0xFECB.");
    }

    if (*pgn > kJ1939MaxPgn) {
        return tr("A PGN has at most 18 bits: it cannot be more than 262143 (0x3FFFF).");
    }

    // A group below 0xF000 is addressed to one ECU, and the last byte of its number is the address
    // it goes to - which a request names as zero, because it is not part of the group.
    const std::uint32_t pduFormat = (*pgn >> 8U) & 0xFFU;
    if (pduFormat < kJ1939Pdu2Threshold && (*pgn & 0xFFU) != 0U) {
        return tr("A group below 0xF000 goes to one ECU and ends in 00 - its last byte is the "
                  "address "
                  "it goes to. Did you mean %1?")
            .arg(pgnText(*pgn & ~0xFFU));
    }

    return {};
}

bool J1939RequestDialog::clearsFaultCodes(std::uint32_t pgn) noexcept
{
    return pgn == kPgnDm3 || pgn == kPgnDm11;
}

J1939RequestDialog::J1939RequestDialog(std::vector<Ecu> ecus,
                                       std::vector<Bus> buses,
                                       QWidget* parent)
    : QDialog{parent}
{
    setWindowTitle(tr("Request a Parameter Group"));
    setModal(true);

    buildUi(ecus, buses);
    onTextChanged();
}

void J1939RequestDialog::buildUi(const std::vector<Ecu>& ecus, const std::vector<Bus>& buses)
{
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    m_group = new QComboBox;
    m_group->setObjectName(QStringLiteral("requestGroup"));

    for (const J1939RequestPreset& preset : presets()) {
        m_group->addItem(QStringLiteral("%1 - %2").arg(preset.label, pgnText(preset.pgn)),
                         static_cast<qulonglong>(preset.pgn));
    }
    m_group->addItem(tr("Another group..."), kAnotherGroup);
    form->addRow(tr("Group"), m_group);

    m_pgn = new QLineEdit;
    m_pgn->setObjectName(QStringLiteral("requestPgn"));
    m_pgn->setPlaceholderText(tr("65227, or 0xFECB"));
    form->addRow(tr("PGN"), m_pgn);

    m_problem = new QLabel;
    m_problem->setObjectName(QStringLiteral("requestProblem"));
    m_problem->setWordWrap(true);

    // Room for the longest thing it says - two lines - so that the dialog does not have to grow
    // when it says it: a wrapped label does not ask a window that is already shown for more height.
    m_problem->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_problem->setMinimumHeight(2 * m_problem->fontMetrics().lineSpacing());
    setMinimumWidth(480);
    form->addRow(QString{}, m_problem);

    m_destination = new QComboBox;
    m_destination->setObjectName(QStringLiteral("requestDestination"));
    m_destination->addItem(tr("Every ECU"), kEveryEcu);
    for (const Ecu& ecu : ecus) {
        m_destination->addItem(ecu.label, ecu.address);
    }
    form->addRow(tr("ECU"), m_destination);

    m_channel = new QComboBox;
    m_channel->setObjectName(QStringLiteral("requestChannel"));
    m_channel->addItem(tr("Every channel"), kEveryChannel);
    for (const Bus& bus : buses) {
        m_channel->addItem(bus.label, bus.channel);
    }
    form->addRow(tr("Channel"), m_channel);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ask = buttons->button(QDialogButtonBox::Ok);
    m_ask->setText(tr("Ask"));
    connect(buttons, &QDialogButtonBox::accepted, this, &J1939RequestDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_group, &QComboBox::activated, this, &J1939RequestDialog::onPresetChosen);
    connect(m_pgn, &QLineEdit::textChanged, this, &J1939RequestDialog::onTextChanged);

    // Starts on the group asked for most: the claim.
    onPresetChosen(0);
}

void J1939RequestDialog::onPresetChosen(int index)
{
    // The groups the list names carry their number; the one at the end carries none.
    const QVariant entry = m_group->itemData(index);

    if (entry.typeId() == QMetaType::Int) {
        // A group that is not on the list: an empty field to type its number in.
        m_pgn->clear();
        m_pgn->setFocus();
        return;
    }

    m_pgn->setText(QString::number(entry.toULongLong()));
}

void J1939RequestDialog::onTextChanged()
{
    // The list says which group the number is, when it is one the list knows.
    const std::optional<std::uint32_t> typed = parsePgn(m_pgn->text());
    int shown = m_group->count() - 1; // another group

    if (typed.has_value()) {
        for (int index = 0; index < m_group->count() - 1; ++index) {
            if (m_group->itemData(index).toULongLong() == *typed) {
                shown = index;
                break;
            }
        }
    }

    const QSignalBlocker blocker{m_group};
    m_group->setCurrentIndex(shown);

    const QString words = problemWith(m_pgn->text());
    m_problem->setText(words);

    // The quiet colour for what is only not typed yet, and the error one for what is wrong.
    const ThemeManager* themes = ThemeManager::instance();
    const Theme theme = themes != nullptr ? themes->theme() : Theme::dark();
    const bool empty = m_pgn->text().trimmed().isEmpty();

    m_problem->setStyleSheet(
        QStringLiteral("color: %1;").arg((empty ? theme.textMuted : theme.error).name()));

    m_ask->setEnabled(words.isEmpty());
}

void J1939RequestDialog::accept()
{
    const std::optional<std::uint32_t> group = pgn();
    if (!group.has_value()) {
        return;
    }

    if (clearsFaultCodes(*group)) {
        const QMessageBox::StandardButton answer = QMessageBox::warning(
            this,
            tr("Clear fault codes?"),
            tr("This is not a question. Asking an ECU for DM3 or DM11 asks it to clear its fault "
               "codes - the ones that were active, or the ones that are - and what it clears is "
               "gone.\n\nAsk it anyway?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);

        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    QDialog::accept();
}

std::optional<std::uint32_t> J1939RequestDialog::pgn() const
{
    if (!problemWith(m_pgn->text()).isEmpty()) {
        return std::nullopt;
    }

    return parsePgn(m_pgn->text());
}

int J1939RequestDialog::destination() const
{
    return m_destination->currentData().toInt();
}

int J1939RequestDialog::channel() const
{
    return m_channel->currentData().toInt();
}

void J1939RequestDialog::setPgnText(const QString& text)
{
    m_pgn->setText(text);
}

void J1939RequestDialog::setDestination(int address)
{
    int index = m_destination->findData(address);

    if (index < 0) {
        // An ECU the panel has not heard - or a row it was opened from - is still one to ask.
        m_destination->addItem(
            tr("ECU %1 (0x%2)").arg(address).arg(hexDigits(static_cast<std::uint32_t>(address), 2)),
            address);
        index = m_destination->count() - 1;
    }

    m_destination->setCurrentIndex(index);
}

void J1939RequestDialog::setChannel(int channel)
{
    int index = m_channel->findData(channel);

    if (index < 0) {
        m_channel->addItem(tr("CAN %1").arg(channel + 1), channel);
        index = m_channel->count() - 1;
    }

    m_channel->setCurrentIndex(index);
}

bool J1939RequestDialog::canAsk() const
{
    return m_ask->isEnabled();
}

QString J1939RequestDialog::problem() const
{
    return m_problem->text();
}

} // namespace torquebus::ui
