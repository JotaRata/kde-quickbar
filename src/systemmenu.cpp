/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "systemmenu.h"

#include "quickbaractions.h"

#include <KLocalizedString>
#include <QAction>
#include <QMenu>

namespace
{
QAction *addEntry(QMenu *menu, const QString &text, QLatin1String action)
{
    auto *entry = menu->addAction(text);
    entry->setProperty(QuickBarActions::property, QVariant(action));
    return entry;
}
} // namespace

QMenu *SystemMenu::create(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("quickbar-system-menu"));

    // The desktop-focused counterpart of the window menu: system info,
    // settings and the session/power actions, like the macOS Apple menu
    // (deliberately without icons). Session and power entries go through
    // the standard session/system DBus services, so no privileges are
    // needed; Hibernate runs the suspend-then-hibernate flow configured in
    // systemd's sleep.conf (HibernateDelaySec etc.).
    addEntry(menu, i18n("&About This System"), QLatin1String(QuickBarActions::aboutSystem));

    menu->addSeparator();

    addEntry(menu, i18n("&System Settings"), QLatin1String(QuickBarActions::systemSettings));
    addEntry(menu, i18n("&Lock Screen"), QLatin1String(QuickBarActions::lockScreen));

    menu->addSeparator();

    addEntry(menu, i18n("Log &Out"), QLatin1String(QuickBarActions::logout));
    addEntry(menu, i18n("Sl&eep"), QLatin1String(QuickBarActions::sleep));
    addEntry(menu, i18n("&Hibernate"), QLatin1String(QuickBarActions::hibernate));
    addEntry(menu, i18n("&Restart"), QLatin1String(QuickBarActions::restart));
    addEntry(menu, i18n("Sh&ut Down"), QLatin1String(QuickBarActions::shutdown));

    return menu;
}
