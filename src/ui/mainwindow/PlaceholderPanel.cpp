// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "ui/mainwindow/PlaceholderPanel.h"

#include "ui/theme/ThemeManager.h"

#include <QLabel>
#include <QVBoxLayout>

namespace torquebus::ui {

PlaceholderPanel::PlaceholderPanel(QString title,
                                   QString description,
                                   QString iconName,
                                   QString milestone,
                                   QWidget* parent)
    : QWidget{parent}
    , m_title{std::move(title)}
    , m_description{std::move(description)}
    , m_iconName{std::move(iconName)}
    , m_milestone{std::move(milestone)}
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(10);
    layout->addStretch(1);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_iconLabel);

    auto* titleLabel = new QLabel(m_title, this);
    titleLabel->setAlignment(Qt::AlignCenter);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    auto* descriptionLabel = new QLabel(m_description, this);
    descriptionLabel->setObjectName(QStringLiteral("torquebusPlaceholder"));
    descriptionLabel->setAlignment(Qt::AlignCenter);
    descriptionLabel->setWordWrap(true);
    layout->addWidget(descriptionLabel);

    auto* milestoneLabel = new QLabel(tr("Arrives in TorqueBus Studio %1").arg(m_milestone), this);
    milestoneLabel->setObjectName(QStringLiteral("torquebusPlaceholder"));
    milestoneLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(milestoneLabel);

    layout->addStretch(2);

    rebuild();

    if (ThemeManager* themes = ThemeManager::instance()) {
        connect(themes, &ThemeManager::themeChanged, this, [this] { rebuild(); });
    }
}

void PlaceholderPanel::rebuild()
{
    ThemeManager* themes = ThemeManager::instance();
    if (themes == nullptr || m_iconLabel == nullptr) {
        return;
    }

    const QIcon icon = themes->icon(m_iconName, themes->theme().textMuted);
    m_iconLabel->setPixmap(icon.pixmap(QSize{32, 32}));
}

} // namespace torquebus::ui
