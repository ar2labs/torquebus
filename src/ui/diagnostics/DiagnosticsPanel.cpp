// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/diagnostics/DiagnosticsPanel.h"

#include "core/diagnostics/DiagnosticEvent.h"
#include "core/diagnostics/DiagnosticSession.h"
#include "core/diagnostics/UdsTypes.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QStyle>
#include <QVariant>

#include <array>

namespace torquebus::ui {
namespace {

/// 20 Hz, like every other panel that reads shared state on a timer.
constexpr int kRefreshMs = 50;

/// How many exchanges the log keeps. A console is read, not archived - the
/// trace and the .tblog are where a whole session lives.
constexpr int kMaximumRows = 500;

enum Column : int {
    ColumnTime = 0,
    ColumnDirection,
    ColumnBytes,
    ColumnMeaning,
    ColumnCount,
};

struct Shortcut final {
    const char* label;
    const char* hex;
};

/// The requests worth a button, which is the short list somebody types twenty
/// times a day. Everything else is what the text box is for.
constexpr std::array<Shortcut, 8> kShortcuts{{
    {"Default session", "10 01"},
    {"Extended session", "10 03"},
    {"Programming session", "10 02"},
    {"Read VIN", "22 F1 90"},
    {"Read DTCs", "19 02 FF"},
    {"Clear DTCs", "14 FF FF FF"},
    {"ECU reset (hard)", "11 01"},
    {"Tester present", "3E 00"},
}};

[[nodiscard]] Theme currentTheme()
{
    if (const ThemeManager* themes = ThemeManager::instance()) {
        return themes->theme();
    }
    return Theme::dark();
}

/// The ASCII of a response, when it looks like text.
///
/// A VIN comes back as bytes and is read as characters, and a console that
/// shows only hex makes somebody transcribe seventeen of them by hand. Anything
/// that is not printable is left out of this entirely rather than shown as dots
/// - a half-decoded string invites being read as a whole one.
[[nodiscard]] QString asciiOf(const std::vector<std::uint8_t>& bytes, std::size_t from)
{
    if (bytes.size() <= from) {
        return {};
    }

    QString text;
    text.reserve(static_cast<qsizetype>(bytes.size() - from));

    for (std::size_t index = from; index < bytes.size(); ++index) {
        const std::uint8_t byte = bytes[index];

        if (byte < 0x20 || byte > 0x7E) {
            return {};
        }

        text.append(QLatin1Char{static_cast<char>(byte)});
    }

    // One or two printable bytes are a coincidence, not a string.
    return text.size() >= 4 ? text : QString{};
}

} // namespace

DiagnosticsPanel::DiagnosticsPanel(QWidget* parent)
    : QWidget{parent}
{
    buildUi();

    auto* timer = new QTimer(this);
    timer->setInterval(kRefreshMs);
    connect(timer, &QTimer::timeout, this, &DiagnosticsPanel::refresh);
    timer->start();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { onThemeChanged(); });
    }

    updateAvailability();
}

void DiagnosticsPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    // --- Who is being talked to -------------------------------------------
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);

    m_targetLabel = new QLabel;
    m_targetLabel->setProperty("torquebusRole", QStringLiteral("caption"));
    m_targetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(m_targetLabel);

    header->addStretch(1);

    m_sessionLabel = new QLabel;
    m_sessionLabel->setProperty("torquebusRole", QStringLiteral("caption"));
    header->addWidget(m_sessionLabel);

    layout->addLayout(header);

    // --- What to ask -------------------------------------------------------
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    m_shortcuts = new QComboBox;
    m_shortcuts->addItem(tr("Common requests..."), QString{});

    for (const Shortcut& shortcut : kShortcuts) {
        m_shortcuts->addItem(QString::fromLatin1(shortcut.label),
                             QString::fromLatin1(shortcut.hex));
    }

    m_shortcuts->setToolTip(tr("Fills the box below. Nothing is sent until you press "
                               "Send - a request that fires from a menu is one somebody "
                               "sends by accident."));

    connect(m_shortcuts, &QComboBox::currentIndexChanged, this, &DiagnosticsPanel::onShortcut);
    row->addWidget(m_shortcuts);

    m_request = new QLineEdit;
    m_request->setPlaceholderText(tr("22 F1 90"));
    m_request->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_request->setClearButtonEnabled(true);

    connect(m_request, &QLineEdit::textChanged, this, &DiagnosticsPanel::onRequestChanged);
    connect(m_request, &QLineEdit::returnPressed, this, &DiagnosticsPanel::onSend);

    row->addWidget(m_request, 1);

    m_send = new QPushButton(tr("&Send"));
    m_send->setDefault(true);
    connect(m_send, &QPushButton::clicked, this, &DiagnosticsPanel::onSend);
    row->addWidget(m_send);

    m_clear = new QPushButton(tr("Clear log"));
    m_clear->setAutoDefault(false);
    connect(m_clear, &QPushButton::clicked, this, &DiagnosticsPanel::onClear);
    row->addWidget(m_clear);

    layout->addLayout(row);

    // --- What happened -----------------------------------------------------
    m_log = new QTableWidget(0, ColumnCount, this);
    m_log->setHorizontalHeaderLabels({tr("Time"), QString{}, tr("Bytes"), tr("Meaning")});

    m_log->verticalHeader()->setVisible(false);
    m_log->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_log->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_log->setShowGrid(false);
    m_log->setAlternatingRowColors(true);
    m_log->horizontalHeader()->setSectionResizeMode(ColumnMeaning, QHeaderView::Stretch);

    layout->addWidget(m_log, 1);
}

void DiagnosticsPanel::setSession(DiagnosticSession* session)
{
    m_session = session;
    updateAvailability();
}

void DiagnosticsPanel::setTarget(const QString& description)
{
    m_targetLabel->setText(description.isEmpty()
                               ? tr("No UDS Client block is running.")
                               : description);
}

void DiagnosticsPanel::updateAvailability()
{
    const bool active = m_session != nullptr && m_session->isActive();

    // Send needs a request as well as somewhere to send it. Both conditions,
    // rather than a button that is enabled and then complains.
    std::vector<std::uint8_t> parsed;
    const bool valid = parseHexBytes(m_request->text().toStdString(), parsed);

    m_send->setEnabled(active && valid && !m_session->isBusy());
    m_shortcuts->setEnabled(active);
    m_request->setEnabled(active);

    if (!active) {
        m_sessionLabel->setText(tr("Drop a UDS Client block and press Start."));
    }

    m_shownAsActive = active;
}

void DiagnosticsPanel::onRequestChanged(const QString& text)
{
    std::vector<std::uint8_t> parsed;
    const bool valid = text.trimmed().isEmpty() || parseHexBytes(text.toStdString(), parsed);

    // Said while it is being typed rather than when Send is pressed: half a hex
    // byte is a mistake somebody can see the moment they make it.
    m_request->setProperty("torquebusState",
                           valid ? QStringLiteral("") : QStringLiteral("error"));

    style()->unpolish(m_request);
    style()->polish(m_request);

    updateAvailability();
}

void DiagnosticsPanel::onShortcut(int index)
{
    if (index <= 0) {
        return;
    }

    m_request->setText(m_shortcuts->itemData(index).toString());

    // Back to the prompt, so the box does not sit showing a request that is no
    // longer what is in the field beside it.
    m_shortcuts->setCurrentIndex(0);
    m_request->setFocus();
}

void DiagnosticsPanel::onSend()
{
    if (m_session == nullptr || !m_session->isActive()) {
        return;
    }

    std::vector<std::uint8_t> request;

    if (!parseHexBytes(m_request->text().toStdString(), request)) {
        return;
    }

    // Shown before it is sent, not after it is answered: the log reads in the
    // order things happened, and an ECU that never answers still leaves the
    // question on screen.
    const Theme theme = currentTheme();

    appendRow(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
              QStringLiteral("TX"),
              QString::fromStdString(toHexBytes(request)),
              QString::fromStdString(describeService(request.front())), theme.tx);

    m_session->postRequest(std::move(request));
    updateAvailability();
}

void DiagnosticsPanel::onClear()
{
    m_log->setRowCount(0);
}

void DiagnosticsPanel::appendRow(const QString& time,
                                 const QString& direction,
                                 const QString& bytes,
                                 const QString& meaning,
                                 const QColor& tint)
{
    // Whether to follow the log is decided *before* the row is added: after it,
    // the scroll bar's maximum has already moved and every read looks like it
    // was at the bottom.
    const QScrollBar* scroll = m_log->verticalScrollBar();
    const bool wasAtBottom = scroll == nullptr || scroll->value() >= scroll->maximum() - 2;

    if (m_log->rowCount() >= kMaximumRows) {
        m_log->removeRow(0);
    }

    const int row = m_log->rowCount();
    m_log->insertRow(row);

    const auto cell = [](const QString& text) {
        auto* item = new QTableWidgetItem(text);
        return item;
    };

    m_log->setItem(row, ColumnTime, cell(time));

    auto* directionItem = cell(direction);
    directionItem->setForeground(tint);
    m_log->setItem(row, ColumnDirection, directionItem);

    auto* bytesItem = cell(bytes);
    bytesItem->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setItem(row, ColumnBytes, bytesItem);

    m_log->setItem(row, ColumnMeaning, cell(meaning));

    if (wasAtBottom) {
        m_log->scrollToBottom();
    }
}

void DiagnosticsPanel::appendExchange(const UdsExchange& exchange)
{
    const Theme theme = currentTheme();

    QString meaning = QString::fromStdString(exchange.describe());

    // The ASCII, when there is any: a VIN is bytes on the wire and letters to
    // the person reading, and making them transcribe seventeen hex pairs is
    // a tool being unhelpful on purpose.
    if (exchange.outcome == UdsExchange::Outcome::Positive) {
        // Past the service byte and whatever echo the service carries: 0x62
        // repeats the two-byte identifier, everything else is taken from byte
        // one.
        const std::size_t from =
            exchange.response.size() > 3 && exchange.response[0] == 0x62 ? 3U : 1U;

        if (const QString text = asciiOf(exchange.response, from); !text.isEmpty()) {
            meaning += QStringLiteral("  \"%1\"").arg(text);
        }
    }

    QColor tint = theme.rx;

    if (exchange.outcome == UdsExchange::Outcome::Negative) {
        tint = theme.warning;
    } else if (exchange.outcome != UdsExchange::Outcome::Positive) {
        // A timeout and a mismatch are both "this did not work", and neither is
        // the ECU refusing - which is a different thing and has to look
        // different.
        tint = theme.error;
    }

    appendRow(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
              QStringLiteral("RX"),
              exchange.response.empty()
                  ? tr("-")
                  : QString::fromStdString(toHexBytes(exchange.response)),
              meaning, tint);
}

void DiagnosticsPanel::refresh()
{
    if (m_session == nullptr) {
        return;
    }

    const bool active = m_session->isActive();

    if (active != m_shownAsActive) {
        updateAvailability();
    }

    if (!active) {
        return;
    }

    for (const UdsExchange& exchange : m_session->takeExchanges()) {
        appendExchange(exchange);
    }

    const std::uint8_t session = m_session->sessionType();

    m_sessionLabel->setText(m_session->isBusy()
                                ? tr("Waiting for the ECU...")
                                : tr("Session: %1").arg(session == 0x01   ? tr("default")
                                                        : session == 0x02 ? tr("programming")
                                                        : session == 0x03 ? tr("extended")
                                                                          : tr("0x%1").arg(
                                                                                session, 2, 16,
                                                                                QLatin1Char('0'))));

    // Send is greyed while an answer is outstanding, because UDS is one
    // question at a time and a second one would only queue behind the first.
    m_send->setEnabled(!m_session->isBusy() && m_request->isEnabled()
                       && !m_request->text().trimmed().isEmpty());
}

void DiagnosticsPanel::onThemeChanged()
{
    // The rows already in the log keep the colours they were written with:
    // repainting them would mean storing the outcome per row, and a console is
    // read now rather than after a theme change.
    update();
}

} // namespace torquebus::ui
