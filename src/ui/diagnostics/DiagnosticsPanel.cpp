// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/diagnostics/DiagnosticsPanel.h"

#include "core/diagnostics/DiagnosticEvent.h"
#include "core/diagnostics/DiagnosticSession.h"
#include "core/diagnostics/UdsServiceCatalog.h"
#include "core/diagnostics/UdsTypes.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QStyle>
#include <QVariant>

#include <string>
#include <vector>

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

    m_service = new QComboBox;
    m_service->addItem(tr("Raw hex"), -1);

    for (const UdsServiceTemplate& service : serviceTemplates()) {
        m_service->addItem(QString::fromUtf8(service.name.data(),
                                             static_cast<qsizetype>(service.name.size())),
                           static_cast<int>(service.service));
    }

    m_service->setToolTip(tr("Fills in the form below and writes the bytes into the box. "
                             "Nothing is sent until you press Send - a request that fires "
                             "from a menu is one somebody sends by accident."));

    connect(m_service, &QComboBox::currentIndexChanged, this,
            &DiagnosticsPanel::onServiceChanged);
    row->addWidget(m_service);

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

    // --- The fields of the chosen service ---------------------------------
    m_form = new QFormLayout;
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(m_form);

    m_hintLabel = new QLabel;
    m_hintLabel->setProperty("torquebusState", QStringLiteral("error"));
    m_hintLabel->setWordWrap(true);
    layout->addWidget(m_hintLabel);

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

    m_send->setEnabled(active && valid && !m_request->text().trimmed().isEmpty()
                       && !m_session->isBusy());
    m_service->setEnabled(active);
    m_request->setEnabled(active);

    if (!active) {
        m_sessionLabel->setText(tr("Drop a UDS Client block and press Start."));
    }

    m_shownAsActive = active;
}

void DiagnosticsPanel::onRequestChanged(const QString& text)
{
    // Typed over by hand: the box wins and the form stops claiming to describe
    // it. Anything else leaves a form and a request on screen that disagree,
    // and only one of them is going to be sent.
    //
    // This only runs for a human edit, because the form writes into the box
    // with the signal blocked - which is also why there is no "am I writing
    // this myself" flag to get wrong.
    if (m_service->currentData().toInt() >= 0) {
        const QSignalBlocker blocker{m_service};
        m_service->setCurrentIndex(0);
        buildForm();
        m_hintLabel->clear();
    }

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

void DiagnosticsPanel::onServiceChanged(int index)
{
    static_cast<void>(index);

    buildForm();
    assembleFromForm();
}

void DiagnosticsPanel::onFieldChanged()
{
    assembleFromForm();
}

void DiagnosticsPanel::buildForm()
{
    // The old editors go with their rows. takeRow leaves the widgets alive and
    // parented to this panel, so they are deleted explicitly rather than left
    // invisible and consuming the field values of a service nobody is looking
    // at any more.
    while (m_form->rowCount() > 0) {
        QFormLayout::TakeRowResult row = m_form->takeRow(0);

        if (row.labelItem != nullptr) {
            delete row.labelItem->widget();
            delete row.labelItem;
        }
        if (row.fieldItem != nullptr) {
            delete row.fieldItem->widget();
            delete row.fieldItem;
        }
    }

    m_fields.clear();

    const int service = m_service->currentData().toInt();

    if (service < 0) {
        return; // Raw hex: the box is the whole interface.
    }

    const UdsServiceTemplate* form = templateFor(static_cast<std::uint8_t>(service));
    if (form == nullptr) {
        return;
    }

    for (const UdsField& field : form->fields) {
        const QString name = QString::fromUtf8(field.name.data(),
                                               static_cast<qsizetype>(field.name.size()));
        const QString help = QString::fromUtf8(field.help.data(),
                                               static_cast<qsizetype>(field.help.size()));
        const QString initial =
            QString::fromUtf8(field.initial.data(),
                              static_cast<qsizetype>(field.initial.size()));

        QWidget* editor = nullptr;

        if (field.kind == UdsField::Kind::SubFunction && !form->choices.empty()) {
            // A named choice rather than a number: "Extended diagnostic" is what
            // somebody means, and 0x03 is how it is spelled.
            auto* box = new QComboBox;

            for (const UdsChoice& choice : form->choices) {
                box->addItem(QStringLiteral("%1  (%2)")
                                 .arg(QString::fromUtf8(
                                          choice.label.data(),
                                          static_cast<qsizetype>(choice.label.size())))
                                 .arg(choice.value, 2, 16, QLatin1Char('0')),
                             QStringLiteral("%1").arg(choice.value, 2, 16, QLatin1Char('0')));
            }

            if (const int found = box->findData(initial); found >= 0) {
                box->setCurrentIndex(found);
            }

            connect(box, &QComboBox::currentIndexChanged, this,
                    &DiagnosticsPanel::onFieldChanged);

            editor = box;
        } else {
            auto* line = new QLineEdit;
            line->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            line->setText(initial);

            if (field.optional) {
                line->setPlaceholderText(tr("optional"));
            }

            connect(line, &QLineEdit::textChanged, this, &DiagnosticsPanel::onFieldChanged);
            editor = line;
        }

        editor->setToolTip(help);

        auto* label = new QLabel(name);
        label->setToolTip(help);

        m_form->addRow(label, editor);
        m_fields.append(editor);
    }
}

void DiagnosticsPanel::assembleFromForm()
{
    const int service = m_service->currentData().toInt();

    if (service < 0) {
        return;
    }

    const UdsServiceTemplate* form = templateFor(static_cast<std::uint8_t>(service));
    if (form == nullptr) {
        return;
    }

    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(m_fields.size()));

    for (QWidget* editor : m_fields) {
        if (const auto* box = qobject_cast<QComboBox*>(editor); box != nullptr) {
            values.push_back(box->currentData().toString().toStdString());
        } else if (const auto* line = qobject_cast<QLineEdit*>(editor); line != nullptr) {
            values.push_back(line->text().toStdString());
        } else {
            values.emplace_back();
        }
    }

    std::vector<std::uint8_t> request;
    const Result result = buildRequest(*form, values, request);

    // A form that is not finished writes nothing rather than half a request:
    // the box would otherwise show bytes that are not what the fields say, and
    // the box is what gets sent.
    const QSignalBlocker blocker{m_request};

    m_request->setText(result.succeeded()
                           ? QString::fromStdString(toHexBytes(request))
                           : QString{});

    if (result.failed()) {
        // Named, and while it is being typed: "Identifier: 1 byte(s) given, 2
        // needed" is the sentence that ends the question.
        m_hintLabel->setText(QString::fromStdString(std::string{result.message()}));
    } else {
        m_hintLabel->clear();
    }

    updateAvailability();
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
