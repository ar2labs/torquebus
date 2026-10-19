// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/dashboard/DashboardWidgetEditor.h"

#include "core/dashboard/DashboardAssets.h"
#include "core/dashboard/cluster/ClusterProfiles.h"
#include "core/plot/SignalSeries.h"
#include "ui/dashboard/DashboardText.h"
#include "ui/dashboard/SignalPickerDialog.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringListModel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace torquebus::ui {
namespace {

/// The three sources, in the combo box, with the enum value beside each.
constexpr int kSourceNone = 0;
constexpr int kSourceSignal = 1;
constexpr int kSourceVariable = 2;

/// The columns of the table of states.
constexpr int kStateValue = 0;
constexpr int kStateLabel = 1;
constexpr int kStateTone = 2;
constexpr int kStatePicture = 3;

[[nodiscard]] int indexOf(DashboardBinding::Source source)
{
    switch (source) {
    case DashboardBinding::Source::Signal:
        return kSourceSignal;
    case DashboardBinding::Source::Variable:
        return kSourceVariable;
    case DashboardBinding::Source::Message:
    case DashboardBinding::Source::None:
        break;
    }

    return kSourceNone;
}

[[nodiscard]] bool isReader(DashboardWidgetKind kind)
{
    const std::vector<DashboardWidgetKind> readers = readerKinds();

    return std::find(readers.begin(), readers.end(), kind) != readers.end();
}

[[nodiscard]] QString amber(const QString& text)
{
    const ThemeManager* themes = ThemeManager::instance();
    const QColor colour = themes != nullptr ? themes->theme().lampAmber : QColor{0xE0, 0xA0, 0x20};

    return QStringLiteral("<span style=\"color:%1\">%2</span>")
        .arg(colour.name(), text.toHtmlEscaped());
}

[[nodiscard]] QDoubleSpinBox* makeSpin()
{
    auto* spin = new QDoubleSpinBox;
    spin->setRange(-1e9, 1e9);
    spin->setDecimals(3);

    // A box that can show -1000000000.000 asks for room to, and the dock this sits in is narrow: it
    // may be squeezed, and is given what it wants when there is some.
    spin->setMinimumWidth(72);
    return spin;
}

} // namespace

DashboardWidgetEditor::DashboardWidgetEditor(DashboardDescription& dashboard, QWidget* parent)
    : QWidget{parent}
    , m_dashboard{dashboard}
{
    buildUi();
    clear();
}

DashboardWidgetEditor::~DashboardWidgetEditor() = default;

void DashboardWidgetEditor::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 6);
    outer->setSpacing(6);

    m_placeholder = new QLabel(tr("Switch the dashboard to Edit and select a widget."));
    m_placeholder->setProperty("torquebusRole", QStringLiteral("caption"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    outer->addWidget(m_placeholder, 1);

    m_form = new QWidget;
    m_layout = new QFormLayout(m_form);
    m_layout->setContentsMargins(0, 0, 4, 0);
    m_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // Tall for the kinds with a lot to say, and the dock is not: the form scrolls.
    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidget(m_form);
    outer->addWidget(m_scroll, 1);

    m_kindLabel = new QLabel;
    m_kindLabel->setObjectName(QStringLiteral("kindLabel"));
    m_layout->addRow(tr("Widget"), m_kindLabel);

    // Any widget that reads one value can be shown as any other that does: a level as a dial, a
    // dial as a trend. The names, the range and the unit are the same; only the drawing is not.
    m_showAs = new QComboBox;
    m_showAs->setObjectName(QStringLiteral("showAs"));
    for (const DashboardWidgetKind kind : readerKinds()) {
        m_showAs->addItem(displayNameOf(kind), static_cast<int>(kind));
    }
    m_showAs->setToolTip(
        tr("What the widget looks like. The signal, the unit and the range stay."));
    m_layout->addRow(tr("Show as"), m_showAs);

    // The cluster shows dozens of values and is bound to none of them: what it asks for instead is
    // a profile, and the registered ones are the choices. A profile registered later - by a plugin
    // - is listed the next time the editor is built.
    m_profile = new QComboBox;
    m_profile->setObjectName(QStringLiteral("profile"));
    for (const ClusterProfile& profile : ClusterProfiles::instance().all()) {
        m_profile->addItem(QString::fromStdString(profile.name),
                           QString::fromStdString(profile.id));
    }
    m_profile->setToolTip(tr("Where each value on the cluster comes from. A value the profile has "
                             "no source for is shown as dashes until one is available."));
    m_layout->addRow(tr("Data profile"), m_profile);

    m_source = new QComboBox;
    m_source->setObjectName(QStringLiteral("source"));
    m_source->addItem(tr("Nothing"), kSourceNone);
    m_source->addItem(tr("CAN signal"), kSourceSignal);
    m_source->addItem(tr("Variable"), kSourceVariable);
    m_layout->addRow(tr("Shows"), m_source);

    // --- The message, with the way to browse for it --------------------------------------------
    m_messageRow = new QWidget;
    auto* messageLayout = new QHBoxLayout(m_messageRow);
    messageLayout->setContentsMargins(0, 0, 0, 0);
    messageLayout->setSpacing(4);

    m_message = new QLineEdit;
    m_message->setObjectName(QStringLiteral("message"));
    m_message->setPlaceholderText(tr("Message name, as the database spells it"));
    messageLayout->addWidget(m_message, 1);

    m_browse = new QPushButton(tr("Browse..."));
    m_browse->setObjectName(QStringLiteral("browse"));
    m_browse->setToolTip(tr("Search the signals of the loaded databases."));
    messageLayout->addWidget(m_browse);

    m_layout->addRow(tr("Message"), m_messageRow);

    m_signal = new QLineEdit;
    m_signal->setObjectName(QStringLiteral("signal"));
    m_signal->setPlaceholderText(tr("Signal name"));
    m_layout->addRow(tr("Signal"), m_signal);

    m_variable = new QLineEdit;
    m_variable->setObjectName(QStringLiteral("variable"));
    m_variable->setPlaceholderText(tr("brake_pedal"));
    m_variable->setToolTip(tr("A name a Lua script reads with var_get() and writes "
                              "with var_set(). It does not have to exist yet."));
    m_layout->addRow(tr("Variable"), m_variable);

    // --- What the databases say about it -------------------------------------------------------
    m_bindingInfo = new QLabel;
    m_bindingInfo->setObjectName(QStringLiteral("bindingInfo"));
    m_bindingInfo->setWordWrap(true);
    m_bindingInfo->setTextFormat(Qt::RichText);
    m_bindingInfo->setProperty("torquebusRole", QStringLiteral("caption"));
    m_layout->addRow(QString{}, m_bindingInfo);

    m_liveInfo = new QLabel;
    m_liveInfo->setObjectName(QStringLiteral("liveInfo"));
    m_liveInfo->setWordWrap(true);
    m_liveInfo->setProperty("torquebusRole", QStringLiteral("caption"));
    m_layout->addRow(QString{}, m_liveInfo);

    m_fill = new QPushButton(tr("Fill from database"));
    m_fill->setObjectName(QStringLiteral("fill"));
    m_fill->setToolTip(
        tr("Set the unit, the range, the decimals and the names of the values to what "
           "the database says. The title is kept if you wrote it."));
    m_layout->addRow(QString{}, m_fill);

    // --- The rest ------------------------------------------------------------------------------
    m_title = new QLineEdit;
    m_title->setObjectName(QStringLiteral("title"));
    m_title->setPlaceholderText(tr("Taken from the binding when empty"));
    m_layout->addRow(tr("Title"), m_title);

    m_unit = new QLineEdit;
    m_unit->setObjectName(QStringLiteral("unit"));
    m_layout->addRow(tr("Unit"), m_unit);

    m_minimum = makeSpin();
    m_minimum->setObjectName(QStringLiteral("minimum"));
    m_layout->addRow(tr("Minimum"), m_minimum);

    m_maximum = makeSpin();
    m_maximum->setObjectName(QStringLiteral("maximum"));
    m_layout->addRow(tr("Maximum"), m_maximum);

    m_threshold = makeSpin();
    m_threshold->setObjectName(QStringLiteral("threshold"));
    m_threshold->setToolTip(tr("A lamp is on above this; a switch writes the maximum "
                               "when it is turned on and the minimum when it is off."));
    m_layout->addRow(tr("Threshold"), m_threshold);

    m_decimals = new QSpinBox;
    m_decimals->setObjectName(QStringLiteral("decimals"));
    m_decimals->setRange(0, 6);
    m_layout->addRow(tr("Decimals"), m_decimals);

    // --- Alarm limits: four ticks and four numbers ---------------------------------------------
    //
    // A row that spans the form, with the name on the tick: a label column of its own beside a tick
    // and a number was wider than the dock this form sits in, and made every widget's form scroll
    // sideways.
    m_limitsBox = new QWidget;
    auto* limitsLayout = new QVBoxLayout(m_limitsBox);
    limitsLayout->setContentsMargins(0, 0, 0, 0);
    limitsLayout->setSpacing(3);

    auto* limitsTitle = new QLabel(tr("Alarm limits"));
    limitsTitle->setProperty("torquebusRole", QStringLiteral("caption"));
    limitsLayout->addWidget(limitsTitle);

    const char* const limitNames[4] = {
        "warningBelow", "warningAbove", "criticalBelow", "criticalAbove"};
    const QString limitLabels[4] = {
        tr("Warning below"), tr("Warning above"), tr("Critical below"), tr("Critical above")};

    for (int i = 0; i < 4; ++i) {
        auto* row = new QWidget;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);

        m_limitOn[i] = new QCheckBox(limitLabels[i]);
        m_limitOn[i]->setObjectName(
            QStringLiteral("limit.%1.on").arg(QLatin1String{limitNames[i]}));

        m_limitValue[i] = makeSpin();
        m_limitValue[i]->setObjectName(
            QStringLiteral("limit.%1").arg(QLatin1String{limitNames[i]}));

        rowLayout->addWidget(m_limitOn[i], 1);
        rowLayout->addWidget(m_limitValue[i]);

        limitsLayout->addWidget(row);
    }

    m_limitsBox->setToolTip(
        tr("Past a warning limit the widget turns amber, past a critical one red. "
           "A limit that is not ticked is not there."));
    m_layout->addRow(m_limitsBox);

    // --- A trend's window ----------------------------------------------------------------------
    m_windowRow = new QWidget;
    auto* windowLayout = new QHBoxLayout(m_windowRow);
    windowLayout->setContentsMargins(0, 0, 0, 0);

    m_window = new QDoubleSpinBox;
    m_window->setObjectName(QStringLiteral("window"));
    m_window->setRange(1.0, 3600.0);
    m_window->setDecimals(0);
    m_window->setSuffix(tr(" s"));
    windowLayout->addWidget(m_window, 1);

    m_layout->addRow(tr("Window"), m_windowRow);

    // --- How long a signal may be silent -------------------------------------------------------
    m_staleRow = new QWidget;
    auto* staleLayout = new QHBoxLayout(m_staleRow);
    staleLayout->setContentsMargins(0, 0, 0, 0);
    staleLayout->setSpacing(4);

    m_staleOn = new QCheckBox;
    m_staleOn->setObjectName(QStringLiteral("stale.on"));

    m_stale = new QSpinBox;
    m_stale->setObjectName(QStringLiteral("stale"));
    m_stale->setRange(1, 3'600'000);
    m_stale->setSuffix(tr(" ms"));
    m_stale->setSingleStep(100);

    staleLayout->addWidget(m_staleOn);
    staleLayout->addWidget(m_stale, 1);

    m_staleRow->setToolTip(
        tr("When the signal has not been refreshed for this long the widget shows "
           "dashes and says it is stale, instead of going on showing its last "
           "value. Not ticked: it shows the last value for ever."));
    m_layout->addRow(tr("Stale after"), m_staleRow);

    // --- A picture -----------------------------------------------------------------------------
    m_pictureRow = new QWidget;
    auto* pictureLayout = new QVBoxLayout(m_pictureRow);
    pictureLayout->setContentsMargins(0, 0, 0, 0);
    pictureLayout->setSpacing(3);

    m_picture = new QLineEdit;
    m_picture->setObjectName(QStringLiteral("picture"));
    m_picture->setReadOnly(true);
    m_picture->setPlaceholderText(tr("None - drop a PNG on the widget, or Browse"));
    pictureLayout->addWidget(m_picture);

    // Under the name and not beside it: the dock this sits in is narrow.
    auto* pictureButtons = new QHBoxLayout;
    pictureButtons->setSpacing(4);

    auto* browsePicture = new QPushButton(tr("Browse..."));
    browsePicture->setObjectName(QStringLiteral("browsePicture"));
    pictureButtons->addWidget(browsePicture);

    auto* clearPicture = new QPushButton(tr("Clear"));
    clearPicture->setObjectName(QStringLiteral("clearPicture"));
    pictureButtons->addWidget(clearPicture);

    pictureButtons->addStretch(1);
    pictureLayout->addLayout(pictureButtons);

    m_pictureInfo = new QLabel;
    m_pictureInfo->setObjectName(QStringLiteral("pictureInfo"));
    m_pictureInfo->setWordWrap(true);
    m_pictureInfo->setTextFormat(Qt::RichText);
    m_pictureInfo->setProperty("torquebusRole", QStringLiteral("caption"));
    pictureLayout->addWidget(m_pictureInfo);

    m_layout->addRow(tr("Picture"), m_pictureRow);

    // --- The words for the values --------------------------------------------------------------
    m_statesBox = new QWidget;
    auto* statesLayout = new QVBoxLayout(m_statesBox);
    statesLayout->setContentsMargins(0, 0, 0, 0);
    statesLayout->setSpacing(3);

    auto* statesTitle = new QLabel(tr("Named values"));
    statesTitle->setProperty("torquebusRole", QStringLiteral("caption"));
    statesLayout->addWidget(statesTitle);

    m_states = new QTableWidget(0, 4);
    m_states->setObjectName(QStringLiteral("states"));
    m_states->setHorizontalHeaderLabels({tr("Value"), tr("Name"), tr("Meaning"), tr("Picture")});
    m_states->horizontalHeader()->setStretchLastSection(true);
    m_states->horizontalHeader()->setSectionResizeMode(kStateLabel, QHeaderView::Stretch);
    m_states->verticalHeader()->setVisible(false);
    m_states->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_states->setMinimumHeight(110);
    statesLayout->addWidget(m_states);

    auto* statesButtons = new QHBoxLayout;
    statesButtons->setSpacing(4);

    auto* addState = new QPushButton(tr("Add"));
    addState->setObjectName(QStringLiteral("states.add"));
    auto* removeState = new QPushButton(tr("Remove"));
    removeState->setObjectName(QStringLiteral("states.remove"));
    auto* statePicture = new QPushButton(tr("Picture..."));
    statePicture->setObjectName(QStringLiteral("states.picture"));
    auto* statesFromDatabase = new QPushButton(tr("From database"));
    statesFromDatabase->setObjectName(QStringLiteral("states.fromDatabase"));
    statesFromDatabase->setToolTip(tr("Replace the names with the database's value table."));

    statesButtons->addWidget(addState);
    statesButtons->addWidget(removeState);
    statesButtons->addWidget(statePicture);
    statesButtons->addStretch(1);
    statesLayout->addLayout(statesButtons);

    // On a line of its own: four buttons in a row were wider than the dock they sit in.
    statesLayout->addWidget(statesFromDatabase);

    m_layout->addRow(m_statesBox);

    m_hint = new QLabel;
    m_hint->setObjectName(QStringLiteral("hint"));
    m_hint->setWordWrap(true);
    m_hint->setProperty("torquebusRole", QStringLiteral("caption"));
    m_layout->addRow(QString{}, m_hint);

    // --- Completion ----------------------------------------------------------------------------
    m_messageNames = new QStringListModel(this);
    m_messageCompleter = new QCompleter(m_messageNames, this);
    m_messageCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_messageCompleter->setFilterMode(Qt::MatchContains);
    m_message->setCompleter(m_messageCompleter);

    m_signalNames = new QStringListModel(this);
    m_signalCompleter = new QCompleter(m_signalNames, this);
    m_signalCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_signalCompleter->setFilterMode(Qt::MatchContains);
    m_signal->setCompleter(m_signalCompleter);

    // Connected after the line edits' own, which set the text first: a choice from the list is a
    // deliberate act, and is the one moment the form fills in what the database knows.
    connect(m_messageCompleter,
            qOverload<const QString&>(&QCompleter::activated),
            this,
            &DashboardWidgetEditor::onMessageCompleted);
    connect(m_signalCompleter,
            qOverload<const QString&>(&QCompleter::activated),
            this,
            &DashboardWidgetEditor::onSignalCompleted);

    // --- Every field writes on change. A form that needs a Save button is a form somebody leaves
    // half-applied - and the description is the live one, so there is nothing to commit.
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int) { onFieldChanged(); });
    connect(m_profile, &QComboBox::currentIndexChanged, this, [this](int) { onFieldChanged(); });
    connect(m_showAs, &QComboBox::activated, this, [this](int) { onKindChosen(); });

    for (QLineEdit* field : {m_message, m_signal, m_variable, m_title, m_unit}) {
        connect(field, &QLineEdit::textEdited, this, [this](const QString&) { onFieldChanged(); });
    }

    for (QDoubleSpinBox* spin : {m_minimum, m_maximum, m_threshold, m_window}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this](double) { onFieldChanged(); });
    }

    connect(m_decimals, &QSpinBox::valueChanged, this, [this](int) { onFieldChanged(); });

    for (int i = 0; i < 4; ++i) {
        connect(m_limitOn[i], &QCheckBox::toggled, this, [this](bool) { onFieldChanged(); });
        connect(m_limitValue[i], &QDoubleSpinBox::valueChanged, this, [this](double) {
            onFieldChanged();
        });
    }

    connect(m_staleOn, &QCheckBox::toggled, this, [this](bool) { onFieldChanged(); });
    connect(m_stale, &QSpinBox::valueChanged, this, [this](int) { onFieldChanged(); });

    connect(m_browse, &QPushButton::clicked, this, &DashboardWidgetEditor::onBrowse);
    connect(m_fill, &QPushButton::clicked, this, &DashboardWidgetEditor::onFillFromDatabase);
    connect(browsePicture, &QPushButton::clicked, this, &DashboardWidgetEditor::onBrowsePicture);
    connect(clearPicture, &QPushButton::clicked, this, &DashboardWidgetEditor::onClearPicture);
    connect(addState, &QPushButton::clicked, this, &DashboardWidgetEditor::onAddState);
    connect(removeState, &QPushButton::clicked, this, &DashboardWidgetEditor::onRemoveState);
    connect(statePicture, &QPushButton::clicked, this, &DashboardWidgetEditor::onStatePicture);
    connect(statesFromDatabase,
            &QPushButton::clicked,
            this,
            &DashboardWidgetEditor::onStatesFromDatabase);
    connect(m_states, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) {
        onFieldChanged();
    });

    // Whether the signal is arriving, once a second while the form is showing a widget.
    m_liveTimer = new QTimer(this);
    m_liveTimer->setInterval(1000);
    connect(m_liveTimer, &QTimer::timeout, this, &DashboardWidgetEditor::updateLiveStatus);
    m_liveTimer->start();
}

DashboardWidget* DashboardWidgetEditor::current()
{
    return const_cast<DashboardWidget*>(m_dashboard.find(m_widgetId.toStdString()));
}

void DashboardWidgetEditor::setDatabases(
    const std::vector<std::shared_ptr<const CanDatabase>>& databases)
{
    m_dictionary.rebuild(databases);

    // Forces the signal completer to be filled again.
    m_completingFor = QStringLiteral("\x01");

    QStringList messages;
    for (const std::string& name : m_dictionary.messageNames()) {
        messages << QString::fromStdString(name);
    }
    m_messageNames->setStringList(messages);

    if (!m_widgetId.isEmpty()) {
        updateCompleters();
        updateBindingInfo();
    }
}

void DashboardWidgetEditor::setPlotStore(const SignalSeriesStore* store)
{
    m_plots = store;
    updateLiveStatus();
}

void DashboardWidgetEditor::setAssetBase(const QString& directory)
{
    m_images.setBaseDirectory(directory);

    if (const DashboardWidget* widget = current(); widget != nullptr) {
        updatePictureInfo(*widget);
    }
}

void DashboardWidgetEditor::clear()
{
    m_widgetId.clear();
    m_entry = nullptr;

    m_scroll->setVisible(false);
    m_placeholder->setVisible(true);
}

void DashboardWidgetEditor::showWidget(const QString& widgetId)
{
    if (widgetId.isEmpty() || m_dashboard.find(widgetId.toStdString()) == nullptr) {
        clear();
        return;
    }

    m_widgetId = widgetId;

    m_placeholder->setVisible(false);
    m_scroll->setVisible(true);

    reload();
}

void DashboardWidgetEditor::reload()
{
    const DashboardWidget* widget = current();
    if (widget == nullptr) {
        clear();
        return;
    }

    // Nothing set below is written back: see onFieldChanged.
    const QScopedValueRollback<bool> populating{m_populating, true};

    m_kindLabel->setText(displayNameOf(widget->kind) + QStringLiteral("  ·  ") + m_widgetId);

    const int shownAs = m_showAs->findData(static_cast<int>(widget->kind));
    m_showAs->setCurrentIndex(shownAs);

    // -1 for a profile the registry does not have, which validate() refuses at load but a widget
    // can still be in if the registry lost it since: the combo then shows nothing rather than
    // another profile's name.
    m_profile->setCurrentIndex(m_profile->findData(QString::fromStdString(widget->profile)));

    m_source->setCurrentIndex(indexOf(widget->binding.source));

    m_message->setText(QString::fromStdString(widget->binding.message));
    m_signal->setText(QString::fromStdString(widget->binding.signal));
    m_variable->setText(QString::fromStdString(widget->binding.variable));

    m_title->setText(QString::fromStdString(widget->title));
    m_unit->setText(QString::fromStdString(widget->unit));

    m_minimum->setValue(widget->minimum);
    m_maximum->setValue(widget->maximum);
    m_threshold->setValue(widget->threshold);
    m_decimals->setValue(widget->decimals);

    // A limit that is not set shows a number that would be sensible if it were, so that ticking it
    // does not start from zero.
    const std::optional<double> limits[4] = {widget->limits.warningBelow,
                                             widget->limits.warningAbove,
                                             widget->limits.criticalBelow,
                                             widget->limits.criticalAbove};
    const double span = widget->maximum - widget->minimum;
    const double suggested[4] = {widget->minimum + span * 0.15,
                                 widget->minimum + span * 0.80,
                                 widget->minimum + span * 0.05,
                                 widget->minimum + span * 0.90};

    for (int i = 0; i < 4; ++i) {
        m_limitOn[i]->setChecked(limits[i].has_value());
        m_limitValue[i]->setValue(limits[i].value_or(suggested[i]));
        m_limitValue[i]->setEnabled(limits[i].has_value());
    }

    m_window->setValue(widget->windowSeconds);

    m_staleOn->setChecked(widget->staleMs > 0);
    m_stale->setValue(widget->staleMs > 0
                          ? static_cast<int>(std::min<std::uint32_t>(widget->staleMs, 3'600'000U))
                          : 1000);
    m_stale->setEnabled(widget->staleMs > 0);

    m_picture->setText(QString::fromStdString(widget->image));
    updatePictureInfo(*widget);

    loadStates(*widget);

    updateCompleters();
    updateBindingInfo();
    updateLiveStatus();
    applyVisibility(*widget);
}

void DashboardWidgetEditor::applyVisibility(const DashboardWidget& widget)
{
    const DashboardKindTraits& traits = traitsOf(widget.kind);

    const bool isCluster = widget.kind == DashboardWidgetKind::Cluster;
    const bool isTable = traits.binding == BindingNeed::Message;

    // A table has a message and nothing else to choose; a label, a group and a cluster read
    // nothing.
    const bool readsAValue =
        traits.binding == BindingNeed::Required || traits.binding == BindingNeed::Optional;

    const bool control = writesItsBinding(widget.kind);
    const bool signalBound = widget.binding.source == DashboardBinding::Source::Signal;
    const bool variableBound = widget.binding.source == DashboardBinding::Source::Variable;

    const auto showRow = [this](QWidget* field, bool visible) {
        // Row, not field: hiding the editor alone leaves its label behind,
        // pointing at nothing.
        if (QWidget* label = m_layout->labelForField(field); label != nullptr) {
            label->setVisible(visible);
        }
        field->setVisible(visible);
    };

    showRow(m_showAs, isReader(widget.kind));
    showRow(m_profile, isCluster);

    // A cluster draws no caption, so a field for one would be a field that does nothing.
    showRow(m_title, !isCluster);

    showRow(m_source, readsAValue);
    showRow(m_messageRow, (readsAValue && signalBound) || isTable);
    showRow(m_signal, readsAValue && signalBound);
    showRow(m_variable, readsAValue && variableBound);

    const bool showBindingInfo = (readsAValue && signalBound) || isTable;
    showRow(m_bindingInfo, showBindingInfo && !m_bindingInfo->text().isEmpty());
    showRow(m_liveInfo, readsAValue && signalBound && !m_liveInfo->text().isEmpty());
    showRow(m_fill, readsAValue && signalBound && m_entry != nullptr);

    showRow(m_unit, traits.unit);
    showRow(m_minimum, traits.range);
    showRow(m_maximum, traits.range);
    showRow(m_threshold, traits.threshold);
    showRow(m_decimals, traits.decimals);
    showRow(m_limitsBox, traits.limits);
    showRow(m_windowRow, traits.window);

    // A time-out is about a signal that arrives: a variable does not stop, and a table is many.
    showRow(m_staleRow, readsAValue && signalBound);

    showRow(m_pictureRow, traits.image);
    showRow(m_statesBox, traits.states);

    // The picture of a state is for the kinds that can show one.
    m_states->setColumnHidden(kStatePicture, !traits.image);

    // The one refusal worth explaining rather than only enforcing: a control
    // bound to a signal moves under the mouse and changes nothing on the bus.
    QString hint;

    if (control && signalBound) {
        hint = tr("A control cannot write a CAN signal. Bind it to a variable "
                  "and let a script or a transmit entry put that on the bus.");
    }

    // Two names for one value is a table that answers differently depending on the row it reads
    // first.
    if (traits.states) {
        std::set<double> seen;

        for (const DashboardState& state : widget.states) {
            if (!seen.insert(state.value).second) {
                hint = tr("Two names have the value %1. Each value can have only one.")
                           .arg(state.value);
                break;
            }
        }
    }

    // A fault list has a title and nothing else to set, and a form with one field leaves the
    // question that matters unanswered: where do its codes come from.
    if (widget.kind == DashboardWidgetKind::Faults) {
        hint = tr("Lists the trouble codes the J1939 block says the bus is reporting, worst first, "
                  "under the four DM1 lamps of the whole bus. It needs a J1939 block in the "
                  "pipeline; without one, or with no measurement running, it is an empty list.");
    }

    m_hint->setText(hint);
    showRow(m_hint, !hint.isEmpty());
}

// ---------------------------------------------------------------------------
// What the databases say
// ---------------------------------------------------------------------------

void DashboardWidgetEditor::updateCompleters()
{
    const QString message = m_message->text().trimmed();

    // What the signal field completes from: the signals of the message when it is one, and every
    // signal, qualified, when it is not - so that "engines" typed into the signal field finds
    // EEC1.EngineSpeed without the message having been typed first.
    const QString completingFor =
        m_dictionary.hasMessage(message.toStdString()) ? message : QString{};

    if (completingFor == m_completingFor) {
        return;
    }

    m_completingFor = completingFor;

    QStringList names;

    if (completingFor.isEmpty()) {
        names.reserve(static_cast<qsizetype>(m_dictionary.size()));

        for (const DictionaryEntry& entry : m_dictionary.entries()) {
            names << QString::fromStdString(entry.qualified());
        }
    } else {
        for (const DictionaryEntry* entry : m_dictionary.signalsOf(completingFor.toStdString())) {
            names << QString::fromStdString(entry->signal);
        }
    }

    m_signalNames->setStringList(names);
}

void DashboardWidgetEditor::updateBindingInfo()
{
    m_entry = nullptr;

    QString text;

    if (const DashboardWidget* widget = current(); widget != nullptr) {
        const BindingFinding finding = m_dictionary.check(widget->binding);

        switch (finding.status) {
        case BindingFinding::Status::Found:
            if (finding.entry != nullptr) {
                m_entry = finding.entry;

                text = summaryOf(*finding.entry).toHtmlEscaped();

                if (!finding.entry->comment.empty()) {
                    text += QStringLiteral("<br>")
                            + QString::fromStdString(finding.entry->comment).toHtmlEscaped();
                }
            } else {
                // A message, found.
                text = tr("%n signal(s)",
                          nullptr,
                          static_cast<int>(m_dictionary.signalsOf(widget->binding.message).size()))
                           .toHtmlEscaped();
            }
            break;

        case BindingFinding::Status::UnknownMessage:
        case BindingFinding::Status::UnknownSignal:
            text = amber(describeFinding(finding, widget->binding));
            break;

        case BindingFinding::Status::NotChecked:
            if (m_dictionary.empty()
                && (widget->binding.source == DashboardBinding::Source::Signal
                    || widget->binding.source == DashboardBinding::Source::Message)) {
                text = tr("No database is loaded, so these names are not checked.").toHtmlEscaped();
            }
            break;
        }
    }

    m_bindingInfo->setText(text);

    if (const DashboardWidget* widget = current(); widget != nullptr) {
        applyVisibility(*widget);
    }
}

QString DashboardWidgetEditor::bindingText() const
{
    return m_bindingInfo->text();
}

QString DashboardWidgetEditor::liveText() const
{
    return m_liveInfo->text();
}

void DashboardWidgetEditor::updateLiveStatus()
{
    QString text;

    const DashboardWidget* widget = current();

    if (widget != nullptr && m_plots != nullptr
        && widget->binding.source == DashboardBinding::Source::Signal
        && !widget->binding.message.empty() && !widget->binding.signal.empty()) {
        const SeriesId id = m_plots->find(widget->binding.message + "." + widget->binding.signal);

        double value = 0.0;
        std::uint64_t when = 0;

        if (id == kNoSeries) {
            text = tr("No samples yet. A Signal Plot block has to receive this signal: wire a "
                      "decoder to one.");
        } else if (!m_plots->latest(id, value, when)) {
            text = tr("The signal has no value yet.");
        } else {
            const std::uint64_t newest = m_plots->newestTimestampNs();
            const double behindMs = newest > when ? static_cast<double>(newest - when) / 1e6 : 0.0;

            text = behindMs < 1500.0 ? tr("Arriving: last value %1").arg(value, 0, 'g', 8)
                                     : tr("Silent for %1 s while the rest of the bus went on.")
                                           .arg(behindMs / 1000.0, 0, 'f', 1);
        }
    }

    if (text != m_liveInfo->text()) {
        m_liveInfo->setText(text);

        if (widget != nullptr) {
            applyVisibility(*widget);
        }
    }
}

// ---------------------------------------------------------------------------
// Writing the widget
// ---------------------------------------------------------------------------

void DashboardWidgetEditor::onFieldChanged()
{
    if (m_populating) {
        return;
    }

    DashboardWidget* widget = current();

    if (widget == nullptr) {
        return;
    }

    const DashboardKindTraits& traits = traitsOf(widget->kind);

    if (traits.binding == BindingNeed::Message) {
        // A table is bound to a message and cannot be bound to anything else.
        widget->binding.source = DashboardBinding::Source::Message;
        widget->binding.signal.clear();
        widget->binding.variable.clear();
    } else {
        switch (m_source->currentData().toInt()) {
        case kSourceSignal:
            widget->binding.source = DashboardBinding::Source::Signal;
            break;
        case kSourceVariable:
            widget->binding.source = DashboardBinding::Source::Variable;
            break;
        default:
            widget->binding.source = DashboardBinding::Source::None;
            break;
        }

        widget->binding.signal = m_signal->text().toStdString();
        widget->binding.variable = m_variable->text().toStdString();
    }

    // Only a cluster has one, and only when the combo has an answer: an empty combo is a registry
    // that lost the profile, not a request to clear it.
    if (widget->kind == DashboardWidgetKind::Cluster && m_profile->currentIndex() >= 0) {
        widget->profile = m_profile->currentData().toString().toStdString();
    }

    widget->binding.message = m_message->text().toStdString();

    widget->title = m_title->text().toStdString();
    widget->unit = m_unit->text().toStdString();

    widget->minimum = m_minimum->value();
    widget->maximum = m_maximum->value();
    widget->threshold = m_threshold->value();
    widget->decimals = m_decimals->value();

    // A limit ticked is a limit set; one that is not is not there, whatever number it shows.
    std::optional<double>* limits[4] = {&widget->limits.warningBelow,
                                        &widget->limits.warningAbove,
                                        &widget->limits.criticalBelow,
                                        &widget->limits.criticalAbove};

    for (int i = 0; i < 4; ++i) {
        m_limitValue[i]->setEnabled(m_limitOn[i]->isChecked());
        *limits[i] = m_limitOn[i]->isChecked() ? std::optional<double>{m_limitValue[i]->value()}
                                               : std::nullopt;
    }

    widget->windowSeconds = m_window->value();

    m_stale->setEnabled(m_staleOn->isChecked());
    widget->staleMs = m_staleOn->isChecked() ? static_cast<std::uint32_t>(m_stale->value()) : 0U;

    if (traits.states) {
        widget->states = statesFromTable();
    }

    updateCompleters();
    updateBindingInfo();
    updateLiveStatus();
    applyVisibility(*widget);

    Q_EMIT widgetEdited(m_widgetId);
}

void DashboardWidgetEditor::bindTo(const DictionaryEntry& entry)
{
    DashboardWidget* widget = current();

    if (widget == nullptr) {
        return;
    }

    applyEntry(*widget, entry);

    // Everything on the form changed under it.
    reload();

    Q_EMIT widgetEdited(m_widgetId);
}

void DashboardWidgetEditor::onBrowse()
{
    const DashboardWidget* widget = current();

    if (widget == nullptr) {
        return;
    }

    if (m_dictionary.empty()) {
        m_bindingInfo->setText(amber(
            tr("No database is loaded. Import a .dbc in the DBC Explorer, then browse it here.")));
        applyVisibility(*widget);
        return;
    }

    const bool table = traitsOf(widget->kind).binding == BindingNeed::Message;

    SignalPickerDialog dialog{m_dictionary,
                              table ? SignalPickerDialog::Mode::ChooseMessage
                                    : SignalPickerDialog::Mode::ChooseSignal,
                              this};

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    if (table) {
        const std::vector<std::string> messages = dialog.chosenMessages();

        if (messages.empty()) {
            return;
        }

        DashboardWidget* editable = current();
        editable->binding.source = DashboardBinding::Source::Message;
        editable->binding.message = messages.front();
        editable->binding.signal.clear();

        reload();
        Q_EMIT widgetEdited(m_widgetId);
        return;
    }

    const std::vector<DictionaryEntry> chosen = dialog.chosenSignals();

    if (!chosen.empty()) {
        bindTo(chosen.front());
    }
}

void DashboardWidgetEditor::onFillFromDatabase()
{
    if (m_entry != nullptr) {
        // Copied: applyEntry changes the widget, and the form is about to be rebuilt.
        const DictionaryEntry entry = *m_entry;
        bindTo(entry);
    }
}

void DashboardWidgetEditor::onKindChosen()
{
    DashboardWidget* widget = current();

    if (widget == nullptr || m_showAs->currentIndex() < 0) {
        return;
    }

    const auto kind = static_cast<DashboardWidgetKind>(m_showAs->currentData().toInt());

    if (kind == widget->kind) {
        return;
    }

    widget->kind = kind;

    // A kind the previous one's range did not describe starts from something that can be swept.
    if (traitsOf(kind).range && !(widget->maximum > widget->minimum)) {
        widget->minimum = 0.0;
        widget->maximum = 100.0;
    }

    // And a signal that the database knows brings what the new kind uses: a bar wants the range, a
    // state wants the names.
    if (m_entry != nullptr && widget->binding.source == DashboardBinding::Source::Signal) {
        const DictionaryEntry entry = *m_entry;
        applyEntry(*widget, entry);
    }

    reload();
    Q_EMIT widgetEdited(m_widgetId);
}

void DashboardWidgetEditor::onMessageCompleted(const QString& text)
{
    Q_UNUSED(text)

    // The line edit has taken the text; what it names is checked like anything typed.
    onFieldChanged();
}

void DashboardWidgetEditor::onSignalCompleted(const QString& text)
{
    // "Message.Signal" when the message was not known when it was chosen, a bare name when it was.
    QString message = m_message->text().trimmed();
    QString signal = text;

    if (const qsizetype dot = text.indexOf(QLatin1Char('.')); dot > 0) {
        message = text.left(dot);
        signal = text.mid(dot + 1);
    }

    if (const DictionaryEntry* entry =
            m_dictionary.find(message.toStdString(), signal.toStdString());
        entry != nullptr) {
        // The whole entry is copied: bindTo changes the widget and rebuilds the form.
        const DictionaryEntry chosen = *entry;
        bindTo(chosen);
        return;
    }

    onFieldChanged();
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

QString DashboardWidgetEditor::choosePicture()
{
    QStringList filters;

    for (const std::string_view extension : imageExtensions()) {
        filters << QStringLiteral("*.")
                       + QString::fromLatin1(extension.data(),
                                             static_cast<qsizetype>(extension.size()));
    }

    const QString file = QFileDialog::getOpenFileName(
        this,
        tr("Choose a picture"),
        m_images.baseDirectory(),
        tr("Pictures (%1);;All files (*)").arg(filters.join(QLatin1Char(' '))));

    if (file.isEmpty()) {
        return {};
    }

    return QString::fromStdString(
        storableAsset(file.toStdString(), m_images.baseDirectory().toStdString()));
}

void DashboardWidgetEditor::updatePictureInfo(const DashboardWidget& widget)
{
    QString text;

    if (!widget.image.empty()) {
        if (const QString problem = m_images.problemWith(widget.image); !problem.isEmpty()) {
            text = amber(problem);
        } else {
            const QSize size = m_images.sizeOf(widget.image);

            text = tr("%1 × %2 px").arg(size.width()).arg(size.height()).toHtmlEscaped();
        }
    }

    m_pictureInfo->setText(text);
    m_pictureInfo->setVisible(!text.isEmpty());
}

void DashboardWidgetEditor::onBrowsePicture()
{
    DashboardWidget* widget = current();

    if (widget == nullptr) {
        return;
    }

    const QString file = choosePicture();

    if (file.isEmpty()) {
        return;
    }

    widget->image = file.toStdString();
    m_picture->setText(file);
    updatePictureInfo(*widget);

    Q_EMIT widgetEdited(m_widgetId);
}

void DashboardWidgetEditor::onClearPicture()
{
    DashboardWidget* widget = current();

    if (widget == nullptr || widget->image.empty()) {
        return;
    }

    widget->image.clear();
    m_picture->clear();
    updatePictureInfo(*widget);

    Q_EMIT widgetEdited(m_widgetId);
}

// ---------------------------------------------------------------------------
// The words for the values
// ---------------------------------------------------------------------------

void DashboardWidgetEditor::addStateRow(const DashboardState& state)
{
    const int row = m_states->rowCount();
    m_states->insertRow(row);

    auto* value = new QDoubleSpinBox;
    value->setRange(-1e9, 1e9);
    value->setDecimals(4);
    value->setFrame(false);
    value->setValue(state.value);
    m_states->setCellWidget(row, kStateValue, value);
    connect(value, &QDoubleSpinBox::valueChanged, this, [this](double) { onFieldChanged(); });

    m_states->setItem(row, kStateLabel, new QTableWidgetItem(QString::fromStdString(state.label)));

    auto* tone = new QComboBox;
    for (const DashboardTone candidate : {DashboardTone::Neutral,
                                          DashboardTone::Ok,
                                          DashboardTone::Info,
                                          DashboardTone::Warning,
                                          DashboardTone::Danger,
                                          DashboardTone::Off}) {
        tone->addItem(displayNameOf(candidate), static_cast<int>(candidate));
    }
    tone->setCurrentIndex(tone->findData(static_cast<int>(state.tone)));
    m_states->setCellWidget(row, kStateTone, tone);
    connect(tone, &QComboBox::currentIndexChanged, this, [this](int) { onFieldChanged(); });

    m_states->setItem(
        row, kStatePicture, new QTableWidgetItem(QString::fromStdString(state.image)));
}

void DashboardWidgetEditor::loadStates(const DashboardWidget& widget)
{
    m_states->setRowCount(0);

    for (const DashboardState& state : widget.states) {
        addStateRow(state);
    }
}

std::vector<DashboardState> DashboardWidgetEditor::statesFromTable() const
{
    std::vector<DashboardState> states;

    for (int row = 0; row < m_states->rowCount(); ++row) {
        const auto* value =
            qobject_cast<const QDoubleSpinBox*>(m_states->cellWidget(row, kStateValue));
        const auto* tone = qobject_cast<const QComboBox*>(m_states->cellWidget(row, kStateTone));

        if (value == nullptr || tone == nullptr) {
            continue;
        }

        DashboardState state;
        state.value = value->value();
        state.tone = static_cast<DashboardTone>(tone->currentData().toInt());

        if (const QTableWidgetItem* label = m_states->item(row, kStateLabel); label != nullptr) {
            state.label = label->text().toStdString();
        }

        if (const QTableWidgetItem* picture = m_states->item(row, kStatePicture);
            picture != nullptr) {
            state.image = picture->text().trimmed().toStdString();
        }

        states.push_back(std::move(state));
    }

    return states;
}

void DashboardWidgetEditor::onAddState()
{
    DashboardWidget* widget = current();

    if (widget == nullptr) {
        return;
    }

    DashboardState state;

    // The next value after the last one, so that adding several gives a table and not a collision.
    state.value = widget->states.empty() ? 0.0 : widget->states.back().value + 1.0;
    state.label = tr("State %1").arg(widget->states.size() + 1).toStdString();

    widget->states.push_back(state);

    {
        const QScopedValueRollback<bool> populating{m_populating, true};
        addStateRow(state);
    }

    applyVisibility(*widget);
    Q_EMIT widgetEdited(m_widgetId);
}

void DashboardWidgetEditor::onRemoveState()
{
    DashboardWidget* widget = current();

    if (widget == nullptr) {
        return;
    }

    const QList<QTableWidgetSelectionRange> ranges = m_states->selectedRanges();

    if (ranges.isEmpty()) {
        return;
    }

    // From the bottom, so that removing a row does not move the next one under the index.
    std::vector<int> rows;

    for (const QTableWidgetSelectionRange& range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            rows.push_back(row);
        }
    }

    std::sort(rows.begin(), rows.end(), std::greater<int>{});
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    {
        const QScopedValueRollback<bool> populating{m_populating, true};

        for (const int row : rows) {
            m_states->removeRow(row);
        }
    }

    widget->states = statesFromTable();

    applyVisibility(*widget);
    Q_EMIT widgetEdited(m_widgetId);
}

void DashboardWidgetEditor::onStatePicture()
{
    const int row = m_states->currentRow();

    if (row < 0) {
        return;
    }

    const QString file = choosePicture();

    if (file.isEmpty()) {
        return;
    }

    if (QTableWidgetItem* picture = m_states->item(row, kStatePicture); picture != nullptr) {
        // Setting the text fires itemChanged, which writes the table back.
        picture->setText(file);
    }
}

void DashboardWidgetEditor::onStatesFromDatabase()
{
    DashboardWidget* widget = current();

    if (widget == nullptr || m_entry == nullptr) {
        return;
    }

    // Names from the database, and the pictures somebody chose for a value stay with that value.
    std::vector<DashboardState> fresh = statesOf(*m_entry);

    for (DashboardState& state : fresh) {
        if (const DashboardState* before = widget->stateOf(state.value); before != nullptr) {
            state.image = before->image;
        }
    }

    widget->states = std::move(fresh);

    reload();
    Q_EMIT widgetEdited(m_widgetId);
}

} // namespace torquebus::ui
