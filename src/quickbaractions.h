/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// Property-based protocol connecting QuickBar-built menus (the generic
// fallback menu, the application-name window menu) to the task-management
// slots in AppMenuModel. An action carrying QuickBarActions::property set to
// one of the constants below is wired by AppMenuModel::wireMenuActions().
namespace QuickBarActions
{
// Historical property name; kept verbatim for compatibility with existing
// menus (also used by GenericMenu).
constexpr auto property = "quickbarGenericAction";

constexpr auto quit = "quitApplication";
constexpr auto minimize = "minimizeWindow";
constexpr auto maximize = "maximizeWindow";
constexpr auto keepAbove = "keepAboveWindow";
constexpr auto keepBelow = "keepBelowWindow";
constexpr auto fullScreen = "fullScreenWindow";
constexpr auto noBorder = "noBorderWindow";
constexpr auto excludeFromCapture = "excludeFromCaptureWindow";
constexpr auto move = "moveWindow";
constexpr auto resize = "resizeWindow";
constexpr auto windowSettings = "windowSettings";
constexpr auto appSettings = "appSettings";
constexpr auto about = "openAbout";

// System menu (the application-name dropdown while the desktop is focused
// instead of a window): session and power actions.
constexpr auto aboutSystem = "aboutSystem";
constexpr auto systemSettings = "systemSettings";
constexpr auto lockScreen = "lockScreen";
constexpr auto logout = "logout";
constexpr auto sleep = "sleep";
constexpr auto hibernate = "hibernate";
constexpr auto restart = "restart";
constexpr auto shutdown = "shutdown";
} // namespace QuickBarActions
