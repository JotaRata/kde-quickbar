/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

class QMenu;
class QWidget;

namespace WindowMenu
{
// Dropdown opened from the application name label: window/application
// actions (about, minimize, maximize, quit) for the active task.
QMenu *create(QWidget *parent = nullptr);
}
