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
#include <QStringList>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>

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

void ThemeManager::applyVariant(ThemeVariant variant)
{
    m_theme = Theme::forVariant(variant);
    m_iconCache.clear();

    applyFont();
    applyPalette();
    applyStyleSheet();

    Q_EMIT themeChanged(m_theme);
}

void ThemeManager::toggleVariant()
{
    applyVariant(m_theme.variant == ThemeVariant::Dark ? ThemeVariant::Light
                                                       : ThemeVariant::Dark);
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

    return sheet;
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
