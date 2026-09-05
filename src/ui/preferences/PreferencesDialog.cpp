// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/preferences/PreferencesDialog.h"

#include "services/SettingsStore.h"
#include "ui/preferences/AccentSwatchRow.h"
#include "ui/preferences/ThemeCard.h"
#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtGlobal>

namespace torquebus::ui {
namespace {

/// Wide enough for the widest page without a scroll bar, short enough to sit on
/// a laptop screen above the taskbar.
constexpr int kDialogWidth = 720;
constexpr int kDialogHeight = 520;

constexpr int kPageListWidth = 176;

/// A section heading inside a page. Bold and spaced rather than a QGroupBox:
/// a box around three controls is a border the eye has to cross for nothing,
/// and this dialog already has enough edges in it.
[[nodiscard]] QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("preferencesSection"));

    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);

    return label;
}

/// Explanatory text under a control. Wrapped, muted, and never longer than the
/// two lines somebody will actually read.
[[nodiscard]] QLabel* hintLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("preferencesHint"));
    label->setWordWrap(true);
    return label;
}

} // namespace

PreferencesDialog::PreferencesDialog(ThemeManager& themes,
                                     services::SettingsStore& settings,
                                     QWidget* parent)
    : QDialog{parent}
    , m_themes{themes}
    , m_settings{settings}
{
    setWindowTitle(tr("Preferences"));
    setModal(true);
    resize(kDialogWidth, kDialogHeight);

    // Recorded before any control is built, because building them is what will
    // start changing things.
    m_openingVariant = m_themes.variant();
    m_openingAccent = m_themes.accent();
    m_openingDensity = m_themes.density();
    m_openingFollowSystem = m_themes.followsSystemTheme();

    buildUi();
    loadFromSettings();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void PreferencesDialog::buildUi()
{
    m_pageList = new QListWidget(this);
    m_pageList->setObjectName(QStringLiteral("preferencesPages"));
    m_pageList->setFixedWidth(kPageListWidth);
    m_pageList->setFrameShape(QFrame::NoFrame);
    m_pageList->setUniformItemSizes(true);

    m_pages = new QStackedWidget(this);

    const auto addPage = [this](const QString& title, QWidget* page) {
        m_pageList->addItem(title);
        m_pages->addWidget(page);
    };

    addPage(tr("General"), buildGeneralPage());
    addPage(tr("Appearance"), buildAppearancePage());
    addPage(tr("Trace and Transmit"), buildTracePage());
    addPage(tr("Shortcuts"),
            buildPlannedPage(tr("Shortcuts"),
                             tr("A searchable list of every command, with its keys, and a "
                                "place to change them. Conflicts flagged as you type, and "
                                "the whole map exportable so a lab can share one."),
                             QStringLiteral("v0.10")));
    addPage(tr("Plugins"),
            buildPlannedPage(tr("Plugins"),
                             tr("Lua packages that add pipeline blocks, panels and "
                                "database formats. What is installed, what it provides, "
                                "and a switch to turn each one off."),
                             QStringLiteral("v0.10")));

    connect(m_pageList, &QListWidget::currentRowChanged, m_pages,
            &QStackedWidget::setCurrentIndex);

    m_pageList->setCurrentRow(1); // Appearance, which is what this is mostly for.

    auto* reset = new QPushButton(tr("Reset preferences"), this);
    reset->setAutoDefault(false);
    reset->setToolTip(tr("Puts every setting in this dialog back to its default.\n"
                         "The window layout is not touched - use View > Reset Layout "
                         "for that."));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    connect(reset, &QPushButton::clicked, this, &PreferencesDialog::onResetPreferences);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* footer = new QHBoxLayout;
    footer->addWidget(reset);
    footer->addStretch(1);
    footer->addWidget(buttons);

    auto* body = new QHBoxLayout;
    body->setSpacing(12);
    body->addWidget(m_pageList);
    body->addWidget(m_pages, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);
    layout->addLayout(body, 1);
    layout->addLayout(footer);
}

QWidget* PreferencesDialog::buildGeneralPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(sectionLabel(tr("Starting up"), page));

    m_restoreLayout = new QCheckBox(tr("Restore the panel layout from the last session"), page);
    layout->addWidget(m_restoreLayout);
    layout->addWidget(hintLabel(
        tr("Off means every session starts from the default arrangement. The layout is "
           "still saved either way, so turning this back on brings it back."),
        page));

    layout->addSpacing(8);
    layout->addWidget(sectionLabel(tr("Settings file"), page));

    layout->addWidget(hintLabel(
        tr("Preferences are stored as JSON - readable, diffable, and copyable between "
           "machines. Project state lives in the .tbsproj instead."),
        page));

    m_settingsPath = new QLabel(page);
    m_settingsPath->setObjectName(QStringLiteral("preferencesHint"));
    m_settingsPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_settingsPath->setWordWrap(true);
    m_settingsPath->setText(m_settings.filePath());
    layout->addWidget(m_settingsPath);

    auto* open = new QPushButton(tr("Open containing folder"), page);
    open->setAutoDefault(false);
    connect(open, &QPushButton::clicked, this, &PreferencesDialog::onOpenSettingsFolder);

    auto* row = new QHBoxLayout;
    row->addWidget(open);
    row->addStretch(1);
    layout->addLayout(row);

    layout->addStretch(1);

    connect(m_restoreLayout, &QCheckBox::toggled, this, [this](bool) {
        if (!m_loading) {
            storeToSettings();
        }
    });

    return page;
}

QWidget* PreferencesDialog::buildAppearancePage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(sectionLabel(tr("Theme"), page));

    m_lightCard = new ThemeCard(ThemeVariant::Light, page);
    m_darkCard = new ThemeCard(ThemeVariant::Dark, page);

    auto* cards = new QHBoxLayout;
    cards->setSpacing(14);
    cards->addWidget(m_lightCard);
    cards->addWidget(m_darkCard);
    cards->addStretch(1);
    layout->addLayout(cards);

    auto* cardLabels = new QHBoxLayout;
    cardLabels->setSpacing(14);

    auto* lightName = new QLabel(tr("Light"), page);
    lightName->setFixedWidth(m_lightCard->sizeHint().width());
    lightName->setAlignment(Qt::AlignHCenter);

    auto* darkName = new QLabel(tr("Dark"), page);
    darkName->setFixedWidth(m_darkCard->sizeHint().width());
    darkName->setAlignment(Qt::AlignHCenter);

    cardLabels->addWidget(lightName);
    cardLabels->addWidget(darkName);
    cardLabels->addStretch(1);
    layout->addLayout(cardLabels);

    m_followSystem = new QCheckBox(tr("Follow the system theme"), page);
    layout->addWidget(m_followSystem);
    layout->addWidget(hintLabel(
        tr("Picking a theme above turns this off: an explicit choice keeps winning, "
           "rather than being undone the next time Windows changes its mind."),
        page));

    layout->addSpacing(10);
    layout->addWidget(sectionLabel(tr("Accent colour"), page));

    m_accentRow = new AccentSwatchRow(page);

    auto* accentRow = new QHBoxLayout;
    accentRow->addWidget(m_accentRow);
    accentRow->addStretch(1);
    layout->addLayout(accentRow);

    layout->addWidget(hintLabel(
        tr("A swatch stores a hue, not a colour. The tone is worked out for whichever "
           "theme is running and checked for contrast, so one choice is a luminous "
           "colour on Dark and a deep one on Light - the cards above show both."),
        page));

    layout->addSpacing(10);
    layout->addWidget(sectionLabel(tr("Density"), page));

    m_density = new QComboBox(page);
    m_density->addItem(tr("Compact"), toString(Density::Compact));
    m_density->addItem(tr("Comfortable"), toString(Density::Comfortable));
    m_density->addItem(tr("Spacious"), toString(Density::Spacious));
    m_density->setMaximumWidth(200);

    auto* densityRow = new QHBoxLayout;
    densityRow->addWidget(m_density);
    densityRow->addStretch(1);
    layout->addLayout(densityRow);

    layout->addWidget(hintLabel(
        tr("How much air a row of a table gets. Comfortable is what TorqueBus has "
           "always used; Compact fits more of a trace on a laptop screen."),
        page));

    layout->addStretch(1);

    connect(m_lightCard, &ThemeCard::clicked, this, &PreferencesDialog::onThemeCardClicked);
    connect(m_darkCard, &ThemeCard::clicked, this, &PreferencesDialog::onThemeCardClicked);
    connect(m_followSystem, &QCheckBox::toggled, this,
            &PreferencesDialog::onFollowSystemToggled);
    connect(m_accentRow, &AccentSwatchRow::accentChanged, this,
            &PreferencesDialog::onAccentChanged);
    connect(m_density, &QComboBox::currentIndexChanged, this,
            &PreferencesDialog::onDensityChanged);

    return page;
}

QWidget* PreferencesDialog::buildTracePage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(sectionLabel(tr("Trace"), page));

    m_traceRefresh = new QSpinBox(page);
    m_traceRefresh->setRange(10, 500);
    m_traceRefresh->setSingleStep(10);
    m_traceRefresh->setSuffix(tr(" ms"));
    m_traceRefresh->setMaximumWidth(200);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("Refresh the view every"), m_traceRefresh);
    layout->addLayout(form);

    layout->addWidget(hintLabel(
        tr("How often new rows appear - not how often frames are captured. The store "
           "is filled by the pipeline at full rate whatever this says, so a slower "
           "refresh loses nothing but liveness."),
        page));

    layout->addSpacing(10);
    layout->addWidget(sectionLabel(tr("Identifiers"), page));

    m_decimalIdentifiers = new QCheckBox(tr("Show identifiers in decimal"), page);
    layout->addWidget(m_decimalIdentifiers);
    layout->addWidget(hintLabel(
        tr("Hexadecimal by default, because that is how a database, a datasheet and "
           "every other tool in this family writes a CAN identifier. Decimal is "
           "occasionally what a J1939 document uses."),
        page));

    layout->addStretch(1);

    const auto changed = [this] {
        if (m_loading) {
            return;
        }
        storeToSettings();
        Q_EMIT tracePreferencesChanged();
    };

    connect(m_traceRefresh, &QSpinBox::valueChanged, this, [changed](int) { changed(); });
    connect(m_decimalIdentifiers, &QCheckBox::toggled, this, [changed](bool) { changed(); });

    return page;
}

QWidget* PreferencesDialog::buildPlannedPage(const QString& title,
                                             const QString& description,
                                             const QString& milestone)
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(sectionLabel(title, page));
    layout->addWidget(hintLabel(description, page));

    auto* planned = new QLabel(tr("Planned for %1.").arg(milestone), page);
    planned->setObjectName(QStringLiteral("preferencesHint"));
    layout->addWidget(planned);

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Reacting to the controls
// ---------------------------------------------------------------------------

void PreferencesDialog::onThemeCardClicked(ThemeVariant variant)
{
    // An explicit choice, so the manager stops following the desktop. The
    // checkbox is put back in step rather than left claiming otherwise.
    m_themes.applyVariant(variant);

    m_loading = true;
    m_followSystem->setChecked(false);
    m_loading = false;

    applyAppearance();
    storeToSettings();
}

void PreferencesDialog::onFollowSystemToggled(bool follow)
{
    if (m_loading) {
        return;
    }

    m_themes.setFollowSystemTheme(follow);
    applyAppearance();
    storeToSettings();
}

void PreferencesDialog::onAccentChanged(AccentColor accent)
{
    m_themes.setAccent(accent);
    applyAppearance();
    storeToSettings();
}

void PreferencesDialog::onDensityChanged(int /*index*/)
{
    if (m_loading) {
        return;
    }

    m_themes.setDensity(densityFromString(m_density->currentData().toString()));
    storeToSettings();
}

void PreferencesDialog::onResetPreferences()
{
    m_themes.applyPreferences(AccentColor::TorqueBus, Density::Comfortable,
                              /*followSystem=*/true, ThemeVariant::Dark);

    m_loading = true;
    m_restoreLayout->setChecked(kDefaultRestoreLayout);
    m_traceRefresh->setValue(kDefaultTraceRefreshMs);
    m_decimalIdentifiers->setChecked(kDefaultDecimalIdentifiers);
    m_accentRow->setAccent(AccentColor::TorqueBus);
    m_density->setCurrentIndex(m_density->findData(toString(Density::Comfortable)));
    m_followSystem->setChecked(true);
    m_loading = false;

    applyAppearance();
    storeToSettings();

    Q_EMIT tracePreferencesChanged();
}

void PreferencesDialog::onOpenSettingsFolder()
{
    const QFileInfo info{m_settings.filePath()};

    // The folder, not the file: opening settings.json would hand it to whatever
    // the desktop thinks owns .json, which on a developer's machine is an
    // editor and on everybody else's is a dialog asking what to do with it.
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

// ---------------------------------------------------------------------------
// Appearance, settings, and putting things back
// ---------------------------------------------------------------------------

void PreferencesDialog::applyAppearance()
{
    const AccentColor accent = m_themes.accent();
    const ThemeVariant variant = m_themes.variant();

    // The cards preview the *chosen accent* on each theme, so both are
    // repainted whenever either changes.
    m_lightCard->setAccent(accent);
    m_darkCard->setAccent(accent);

    // Nothing is selected while the desktop is deciding. A card marked as
    // chosen under a ticked "Follow the system theme" would be claiming a
    // choice the user has just handed away.
    const bool following = m_themes.followsSystemTheme();

    m_lightCard->setSelected(!following && variant == ThemeVariant::Light);
    m_darkCard->setSelected(!following && variant == ThemeVariant::Dark);
}

void PreferencesDialog::loadFromSettings()
{
    m_loading = true;

    m_restoreLayout->setChecked(m_settings.boolValue(
        QString::fromLatin1(services::keys::kRestoreLayout), kDefaultRestoreLayout));

    m_traceRefresh->setValue(m_settings.intValue(
        QString::fromLatin1(services::keys::kTraceRefreshMs), kDefaultTraceRefreshMs));

    m_decimalIdentifiers->setChecked(m_settings.boolValue(
        QString::fromLatin1(services::keys::kDecimalIdentifiers),
        kDefaultDecimalIdentifiers));

    // Appearance comes from the manager, not the file. The file is what the
    // manager was built from at startup, but the View menu can have changed the
    // theme since - and the dialog must show what is on screen.
    m_accentRow->setAccent(m_themes.accent());
    m_followSystem->setChecked(m_themes.followsSystemTheme());
    m_density->setCurrentIndex(m_density->findData(toString(m_themes.density())));

    m_loading = false;

    // What Cancel puts back. Read off the controls after they were filled in,
    // rather than from the file a second time, so the two can never disagree.
    m_openingTraceRefreshMs = m_traceRefresh->value();
    m_openingDecimalIdentifiers = m_decimalIdentifiers->isChecked();
    m_openingRestoreLayout = m_restoreLayout->isChecked();

    applyAppearance();
}

void PreferencesDialog::storeToSettings()
{
    m_settings.setValue(QString::fromLatin1(services::keys::kTheme),
                        toString(m_themes.variant()));
    m_settings.setValue(QString::fromLatin1(services::keys::kAccent),
                        toString(m_themes.accent()));
    m_settings.setValue(QString::fromLatin1(services::keys::kDensity),
                        toString(m_themes.density()));
    m_settings.setBoolValue(QString::fromLatin1(services::keys::kFollowSystemTheme),
                            m_themes.followsSystemTheme());

    m_settings.setBoolValue(QString::fromLatin1(services::keys::kRestoreLayout),
                            m_restoreLayout->isChecked());
    m_settings.setIntValue(QString::fromLatin1(services::keys::kTraceRefreshMs),
                           m_traceRefresh->value());
    m_settings.setBoolValue(QString::fromLatin1(services::keys::kDecimalIdentifiers),
                            m_decimalIdentifiers->isChecked());

    // Written now rather than at shutdown. A preference the user watched take
    // effect and then lost to a crash is worse than one that never applied,
    // because they have no reason to suspect it went missing.
    if (!m_settings.save()) {
        qWarning("TorqueBus: failed to write settings to %s",
                 qPrintable(m_settings.filePath()));
    }
}

void PreferencesDialog::revertAppearance()
{
    m_themes.applyPreferences(m_openingAccent, m_openingDensity, m_openingFollowSystem,
                              m_openingVariant);
}

void PreferencesDialog::reject()
{
    // Cancel is what makes applying live safe rather than a trap: everything
    // goes back to what was in force when the dialog opened, including what the
    // file says, so a cancelled experiment leaves nothing behind.
    revertAppearance();

    m_loading = true;
    m_restoreLayout->setChecked(m_openingRestoreLayout);
    m_traceRefresh->setValue(m_openingTraceRefreshMs);
    m_decimalIdentifiers->setChecked(m_openingDecimalIdentifiers);
    m_loading = false;

    storeToSettings();

    Q_EMIT tracePreferencesChanged();

    QDialog::reject();
}

int PreferencesDialog::traceRefreshMs() const
{
    return m_traceRefresh->value();
}

bool PreferencesDialog::decimalIdentifiers() const
{
    return m_decimalIdentifiers->isChecked();
}

} // namespace torquebus::ui
