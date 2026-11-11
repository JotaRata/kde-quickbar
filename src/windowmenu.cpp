/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "windowmenu.h"

#include "quickbaractions.h"

#include <KLocalizedString>
#include <QAction>
#include <QIcon>
#include <QMenu>

namespace
{
QAction *addOperation(QMenu *menu, const QString &text, QLatin1String action,
                      const QString &iconName = QString(), bool checkable = false)
{
    auto *operation = menu->addAction(text);
    operation->setProperty(QuickBarActions::property, QVariant(action));
    if (!iconName.isEmpty()) {
        // KWin's window operations menu icon names (useractions.cpp); a name
        // missing from the icon theme simply renders without one, same as
        // KWin itself.
        operation->setIcon(QIcon::fromTheme(iconName));
    }
    operation->setCheckable(checkable);
    return operation;
}
} // namespace

QMenu *WindowMenu::create(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("quickbar-window-menu"));

    // Mirrors KWin's window operations menu (Alt+F3 / titlebar "More
    // Actions"), driven through the task manager instead of KWin internals:
    // capabilities and toggled state come from the TasksModel roles. Icon
    // names match KWin's useractions.cpp. The Desktops and Move-to-Screen
    // submenus are rebuilt dynamically by AppMenuModel (their contents come
    // from VirtualDesktopInfo / the screen list, not static actions).
    auto *desktopsMenu = menu->addMenu(i18n("&Desktops"));
    desktopsMenu->setObjectName(QStringLiteral("quickbar-desktops-menu"));
    desktopsMenu->setIcon(QIcon::fromTheme(QStringLiteral("virtual-desktops")));

    addOperation(menu, i18n("Ma&ximize"), QLatin1String(QuickBarActions::maximize),
                 QStringLiteral("window-maximize"), true);

    auto *screensMenu = menu->addMenu(i18n("Move to &Screen"));
    screensMenu->setObjectName(QStringLiteral("quickbar-screens-menu"));
    screensMenu->setIcon(QIcon::fromTheme(QStringLiteral("computer")));

    addOperation(menu, i18n("Mi&nimize"), QLatin1String(QuickBarActions::minimize),
                 QStringLiteral("window-minimize"));
    addOperation(menu, i18n("Keep &Above Others"), QLatin1String(QuickBarActions::keepAbove),
                 QStringLiteral("window-keep-above"), true);

    // Secondary window operations, nested like KWin's own "&More Actions"
    // submenu (which also holds Move and Resize there).
    auto *moreMenu = menu->addMenu(i18n("&More Actions"));
    moreMenu->setIcon(QIcon::fromTheme(QStringLiteral("overflow-menu")));
    addOperation(moreMenu, i18n("&Move"), QLatin1String(QuickBarActions::move),
                 QStringLiteral("transform-move"));
    addOperation(moreMenu, i18n("&Resize"), QLatin1String(QuickBarActions::resize),
                 QStringLiteral("transform-scale"));
    addOperation(moreMenu, i18n("Keep &Below Others"), QLatin1String(QuickBarActions::keepBelow),
                 QStringLiteral("window-keep-below"), true);
    addOperation(moreMenu, i18n("&Fullscreen"), QLatin1String(QuickBarActions::fullScreen),
                 QStringLiteral("view-fullscreen"), true);
    addOperation(moreMenu, i18n("&No Titlebar and Frame"), QLatin1String(QuickBarActions::noBorder),
                 QStringLiteral("edit-none-border"), true);
    // Wayland-only capability, like KWin's menu (hidden on X11 at state
    // refresh; X11 windows cannot exclude themselves from captures).
    addOperation(moreMenu, i18n("&Hide from Screenshots and Screen Recordings"),
                 QLatin1String(QuickBarActions::excludeFromCapture),
                 QStringLiteral("view-private"), true);

    moreMenu->addSeparator();

    addOperation(moreMenu, i18n("Configure Special &Window Settings…"), QLatin1String(QuickBarActions::windowSettings),
                 QStringLiteral("preferences-system-windows-actions"));
    addOperation(moreMenu, i18n("Configure S&pecial Application Settings…"), QLatin1String(QuickBarActions::appSettings),
                 QStringLiteral("preferences-system-windows-actions"));

    menu->addSeparator();

    addOperation(menu, i18n("About QuickBar"), QLatin1String(QuickBarActions::about),
                 QStringLiteral("help-about"));
    addOperation(menu, i18n("Quit Application"), QLatin1String(QuickBarActions::quit),
                 QStringLiteral("application-exit"));

    return menu;
}
