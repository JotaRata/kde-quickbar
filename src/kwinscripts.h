/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

class QString;

// One-shot KWin scripting bridge used for window operations that the task
// manager does not expose yet: TasksModel::requestSendToOutput() only exists
// since Plasma 6.9, so on older stacks the window menu falls back to a
// transient KWin script loaded over DBus (org.kde.KWin /Scripting). The
// script mirrors KWin's own Window::sendToOutput() placement (the Alt+F3
// "Move to Screen" entry) and runs once, then idles until unloaded.
namespace KWinScripts
{
// Whether the bridge could run right now: a KWin instance on the session
// bus and a Wayland session (on X11 the scripting model's output names do
// not match the panel's QScreen names, so name matching would no-op).
bool isAvailable();

// Send KWin's active window to the output with the given name (e.g.
// "DP-1"), moving transients along like KWin does. Returns false when the
// script could not be installed; KWin-side failures are logged by KWin.
bool sendActiveWindowToOutput(const QString &outputName);
}
