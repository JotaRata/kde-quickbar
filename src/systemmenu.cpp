/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "systemmenu.h"

#include "quickbaractions.h"

#include <KLocalizedString>
#include <QAction>
#include <QIcon>
#include <QMenu>

namespace
{
QAction *addEntry(QMenu *menu, const QString &text, QLatin1String action, const QString &iconName)
{
    auto *entry = menu->addAction(text);
    entry->setProperty(QuickBarActions::property, QVariant(action));
    entry->setIcon(QIcon::fromTheme(iconName));
    return entry;
}
} // namespace

QMenu *SystemMenu::create(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("quickbar-system-menu"));

    // The desktop-focused counterpart of the window menu: system info,
    // settings and the session/power actions, like the macOS Apple menu.
    // Power and session entries go through org.kde.LogoutPrompt (the
    // standard Plasma confirmation dialog) and the screensaver / Solid
    // suspend DBus services, so no privileges are needed.
    addEntry(menu, i18n("&About This System"), QLatin1String(QuickBarActions::aboutSystem),
             QStringLiteral("help-about"));

    menu->addSeparator();

    addEntry(menu, i18n("&System Settings"), QLatin1String(QuickBarActions::systemSettings),
             QStringLiteral("preferences-system"));
    addEntry(menu, i18n("&Lock Screen"), QLatin1String(QuickBarActions::lockScreen),
             QStringLiteral("system-lock-screen"));

    menu->addSeparator();

    addEntry(menu, i18n("Log &Out"), QLatin1String(QuickBarActions::logout),
             QStringLiteral("system-log-out"));
    addEntry(menu, i18n("Sl&eep"), QLatin1String(QuickBarActions::sleep),
             QStringLiteral("system-suspend"));
    addEntry(menu, i18n("&Restart"), QLatin1String(QuickBarActions::restart),
             QStringLiteral("system-reboot"));
    addEntry(menu, i18n("Sh&ut Down"), QLatin1String(QuickBarActions::shutdown),
             QStringLiteral("system-shutdown"));

    return menu;
}
