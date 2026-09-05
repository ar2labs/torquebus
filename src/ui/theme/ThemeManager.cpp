// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QRegularExpression>
#include <QStringList>
#include <QStyleFactory>
#include <QStyleHints>

namespace torquebus::ui {
namespace {

ThemeManager* g_instance = nullptr;

/// The single style sheet template. Colour tokens (@accent, @panel, ...) are
/// substituted at apply time, so both themes share one layout definition and
/// can never drift apart structurally.
constexpr auto kStyleSheetResource = ":/themes/torquebus.qss";

/// Recolours a monochrome SVG by painting `color` through the rendered alpha
/// channel. Cheaper and far more maintainable than shipping two icon sets.
[[nodiscard]] QIcon tintedIcon(const QString& resourcePath, const QColor& color)
{
    const QIcon source{resourcePath};
    if (source.isNull()) {
        return {};
    }

    QIcon result;
    const QList<QSize> sizes{QSize{16, 16}, QSize{24, 24}, QSize{32, 32}};

    for (const QSize& size : sizes) {
        QPixmap pixmap = source.pixmap(size);
        if (pixmap.isNull()) {
            continue;
        }

        QPainter painter{&pixmap};
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), color);
        painter.end();

        result.addPixmap(pixmap);
    }

    return result;
}

} // namespace

ThemeManager::ThemeManager(QObject* parent)
    : QObject{parent}
    , m_theme{Theme::dark()}
{
    if (g_instance == nullptr) {
        g_instance = this;
    }

    // Connected once and for the life of the manager, whether or not the
    // preference is on: the flag is checked when the notification arrives.
    // Connecting and disconnecting as the checkbox moves would be the same
    // behaviour with one more thing able to be out of step.
    if (QStyleHints* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
            if (m_followSystemTheme) {
                rebuild(systemVariant());
            }
        });
    }
}

ThemeManager::~ThemeManager()
{
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

ThemeManager* ThemeManager::instance()
{
    return g_instance;
}

void ThemeManager::rebuild(ThemeVariant variant)
{
    m_theme = Theme::forVariant(variant);

    // After the theme is built, never before: the accent is derived *from* the
    // theme's own saturation and lightness, so it needs the finished palette to
    // borrow them from.
    applyAccent(m_theme, m_accent);

    m_iconCache.clear();

    applyFont();
    applyPalette();
    applyStyleSheet();

    Q_EMIT themeChanged(m_theme);
}

void ThemeManager::applyVariant(ThemeVariant variant)
{
    // The user has said which one they want, so stop asking the desktop.
    m_followSystemTheme = false;
    rebuild(variant);
}

void ThemeManager::toggleVariant()
{
    applyVariant(m_theme.variant == ThemeVariant::Dark ? ThemeVariant::Light
                                                       : ThemeVariant::Dark);
}

void ThemeManager::setAccent(AccentColor accent)
{
    if (accent == m_accent) {
        return;
    }

    m_accent = accent;
    rebuild(m_theme.variant);
}

void ThemeManager::setFollowSystemTheme(bool follow)
{
    m_followSystemTheme = follow;

    if (!follow) {
        return;
    }

    // Applied now rather than at the next notification. A checkbox that agrees
    // to follow the desktop and then leaves the window on the other theme until
    // Windows happens to change looks like a checkbox that does nothing.
    //
    // Unconditional, even when the variant already matches: this is reached
    // from a click, once, and a rebuild that turns out to change nothing costs
    // less than the case where it should have changed something and did not.
    rebuild(systemVariant());
}

void ThemeManager::applyPreferences(AccentColor accent, bool followSystem, ThemeVariant variant)
{
    m_accent = accent;
    m_followSystemTheme = followSystem;

    rebuild(followSystem ? systemVariant() : variant);
}

ThemeVariant ThemeManager::systemVariant()
{
    const QStyleHints* hints = QGuiApplication::styleHints();
    if (hints == nullptr) {
        return ThemeVariant::Dark;
    }

    // Only Light is treated as Light. Unknown - which is what a platform with
    // no such setting reports - falls through to the house default rather than
    // being guessed at.
    return hints->colorScheme() == Qt::ColorScheme::Light ? ThemeVariant::Light
                                                          : ThemeVariant::Dark;
}

void ThemeManager::applyFont() const
{
    // The UI font is part of the density budget, not decoration. Windows
    // defaults to 9pt Segoe UI for applications, but Qt's Fusion style tends
    // to come up a point larger, and one point of font size costs roughly two
    // pixels on every row of every table in the application.
    //
    // Set explicitly rather than inherited, so the window looks the same on a
    // machine whose desktop font has been changed.
    QFont font = QApplication::font();

#ifdef Q_OS_WIN
    const QStringList preferred{QStringLiteral("Segoe UI Variable Text"),
                                QStringLiteral("Segoe UI")};

    const QStringList available = QFontDatabase::families();
    for (const QString& family : preferred) {
        if (available.contains(family)) {
            font.setFamily(family);
            break;
        }
    }

    font.setPointSizeF(9.0);
#endif

    font.setHintingPreference(QFont::PreferDefaultHinting);
    QApplication::setFont(font);
}

void ThemeManager::applyPalette() const
{
    // Fusion is the only built-in style that honours a custom palette
    // identically on every platform. Without it Windows 11 would keep drawing
    // native light-mode chrome underneath a dark style sheet.
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    }

    // Start from the theme's own window colour rather than from a default
    // QPalette. A default-constructed QPalette carries the platform's LIGHT
    // values, and only the roles named below get overwritten - which is where
    // the white line above the tab bars came from: QPalette::Light was still
    // pure white, and Fusion draws every frame, bevel and separator out of the
    // Light / Midlight / Mid / Dark / Shadow group.
    //
    // Those five are not decorative. They are the roles a style reaches for
    // when it needs "slightly lighter than this surface" for a raised edge, and
    // leaving them at their defaults means the widgets a style sheet does not
    // reach keep drawing light-theme chrome inside a dark window.
    QPalette palette{m_theme.background};

    palette.setColor(QPalette::Window, m_theme.background);
    palette.setColor(QPalette::WindowText, m_theme.text);
    palette.setColor(QPalette::Base, m_theme.panel);
    palette.setColor(QPalette::AlternateBase, m_theme.panelAlternate);
    palette.setColor(QPalette::Text, m_theme.text);
    palette.setColor(QPalette::PlaceholderText, m_theme.textMuted);

    palette.setColor(QPalette::Button, m_theme.toolbar);
    palette.setColor(QPalette::ButtonText, m_theme.text);

    palette.setColor(QPalette::ToolTipBase, m_theme.toolbar);
    palette.setColor(QPalette::ToolTipText, m_theme.text);

    palette.setColor(QPalette::Highlight, m_theme.accent);
    palette.setColor(QPalette::HighlightedText, m_theme.textInverted);

    palette.setColor(QPalette::Link, m_theme.accent);
    palette.setColor(QPalette::LinkVisited, m_theme.accentHover);

    // The bevel group. Ordered lightest to darkest in a light theme; in a dark
    // theme the roles keep their names but the values invert, because what a
    // style wants from "Light" is "the raised edge of this surface" and on a
    // dark surface that is a slightly lighter grey, not white.
    palette.setColor(QPalette::Light, m_theme.hover);
    palette.setColor(QPalette::Midlight, m_theme.border);
    palette.setColor(QPalette::Mid, m_theme.border);
    palette.setColor(QPalette::Dark, m_theme.background);
    palette.setColor(QPalette::Shadow, m_theme.background);

    palette.setColor(QPalette::BrightText, m_theme.error);

    palette.setColor(QPalette::Disabled, QPalette::Text, m_theme.textMuted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, m_theme.textMuted);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, m_theme.textMuted);

    QApplication::setPalette(palette);
}

void ThemeManager::applyStyleSheet() const
{
    if (auto* application = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        application->setStyleSheet(buildStyleSheet());
    }
}

QString ThemeManager::buildStyleSheet() const
{
    QFile file{QString::fromLatin1(kStyleSheetResource)};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Not a silent fallback: without the style sheet the application still
        // runs, on the palette alone, and looks close enough to a working theme
        // that the failure is easy to mistake for "my changes did nothing".
        m_styleSheetBytes = -1;
        qWarning("TorqueBus: could not open the style sheet resource %s. "
                 "The application will run on the palette alone. This usually "
                 "means the Qt resource was not rebuilt - re-run CMake configure.",
                 kStyleSheetResource);
        return {};
    }

    QString sheet = QString::fromUtf8(file.readAll());
    m_styleSheetBytes = static_cast<int>(sheet.size());

    const auto substitute = [&sheet](const char* token, const QColor& color) {
        sheet.replace(QLatin1String(token), color.name(QColor::HexRgb));
    };

    substitute("@background", m_theme.background);
    substitute("@panelAlternate", m_theme.panelAlternate);
    substitute("@panel", m_theme.panel);
    substitute("@toolbar", m_theme.toolbar);
    substitute("@tabStrip", m_theme.tabStrip);
    substitute("@separator", m_theme.separator);
    substitute("@divider", m_theme.divider);
    substitute("@border", m_theme.border);
    substitute("@hover", m_theme.hover);
    substitute("@selection", m_theme.selection);
    substitute("@textMuted", m_theme.textMuted);
    substitute("@textInverted", m_theme.textInverted);
    substitute("@text", m_theme.text);
    substitute("@accentHover", m_theme.accentHover);
    substitute("@accent", m_theme.accent);
    substitute("@warning", m_theme.warning);
    substitute("@error", m_theme.error);
    substitute("@success", m_theme.success);

    // Arrow glyphs for scroll buttons and combo boxes. QSS `image:` loads a
    // resource as-is and cannot recolour it, so instead of one icon tinted at
    // runtime there are two baked sets and the theme picks one. The suffix
    // names the background the glyph is drawn *on*, not the glyph's own
    // colour - "on-dark" is the light glyph.
    const QString arrowSuffix = m_theme.variant == ThemeVariant::Dark
        ? QStringLiteral("on-dark")
        : QStringLiteral("on-light");

    const auto arrow = [&arrowSuffix](const char* direction) {
        return QStringLiteral(":/icons/chevron-%1-%2.svg")
            .arg(QString::fromLatin1(direction), arrowSuffix);
    };

    sheet.replace(QLatin1String("@arrowLeft"), arrow("left"));
    sheet.replace(QLatin1String("@arrowRight"), arrow("right"));
    sheet.replace(QLatin1String("@arrowDown"), arrow("down"));

    reportUnsubstitutedTokens(sheet);

    return sheet;
}

void ThemeManager::reportUnsubstitutedTokens(const QString& sheet)
{
    // A token nobody registered is the quietest failure this file can produce.
    // Qt does not report an unknown value in a style sheet - it discards the
    // whole declaration and carries on - so `background-color: @tabStrip;` with
    // no substitution behind it means the widget silently keeps whatever colour
    // it had, and the only symptom is a panel that looks slightly wrong.
    //
    // Adding @tabStrip and @separator to the sheet and forgetting one line in
    // the list above is exactly how that happens, and it would have taken a
    // screenshot and a round trip to notice.
    //
    // So: after every substitution, anything still starting with '@' is a
    // mistake, and it says which one.
    static const QRegularExpression pattern{QStringLiteral("@[A-Za-z][A-Za-z0-9]*")};

    QStringList missing;
    auto matches = pattern.globalMatch(sheet);

    while (matches.hasNext()) {
        const QString token = matches.next().captured();
        if (!missing.contains(token)) {
            missing.append(token);
        }
    }

    if (missing.isEmpty()) {
        return;
    }

    qWarning("Style sheet tokens with no value: %s. Every rule using one has "
             "been discarded by Qt, silently. Register them in "
             "ThemeManager::buildStyleSheet().",
             qUtf8Printable(missing.join(QStringLiteral(", "))));
}

bool ThemeManager::hasIconResource(const QString& name)
{
    return QFile::exists(QStringLiteral(":/icons/%1.svg").arg(name));
}

QIcon ThemeManager::icon(const QString& name) const
{
    if (const auto cached = m_iconCache.constFind(name); cached != m_iconCache.constEnd()) {
        return *cached;
    }

    QIcon result = tintedIcon(QStringLiteral(":/icons/%1.svg").arg(name), m_theme.text);
    m_iconCache.insert(name, result);
    return result;
}

QIcon ThemeManager::icon(const QString& name, const QColor& color) const
{
    const QString key = name + QLatin1Char('#') + color.name(QColor::HexRgb);

    if (const auto cached = m_iconCache.constFind(key); cached != m_iconCache.constEnd()) {
        return *cached;
    }

    QIcon result = tintedIcon(QStringLiteral(":/icons/%1.svg").arg(name), color);
    m_iconCache.insert(key, result);
    return result;
}

} // namespace torquebus::ui
