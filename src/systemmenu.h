/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

class QMenu;
class QWidget;

namespace SystemMenu
{
// macOS-style system dropdown opened from the application name label while
// the desktop (instead of a window) is active: about, settings, session and
// power actions, driven through the standard session DBus services.
QMenu *create(QWidget *parent = nullptr);
}
