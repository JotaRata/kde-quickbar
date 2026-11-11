/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kwinscripts.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QFile>
#include <QGuiApplication>
#include <QSaveFile>
#include <QStandardPaths>

namespace
{
constexpr auto pluginName = "quickbar-send-to-output";
constexpr auto scriptFileName = "quickbar-send-to-output.js";

// Mirrors KWin's Window::sendToOutput() (useractions.cpp "Move to Screen"):
// moveToArea() proportional placement, keepInArea() clamping,
// ensureSpecialStateGeometry() snapping for fullscreen/maximized windows and
// transients following the window. Geometry is assigned as object literals
// ({x, y, width, height}) — KWin's scripting engine registers JS-value
// converters for its geometry types, while writing fields of a geometry
// read from a property is silently ignored.
QString buildScript(const QString &outputName)
{
    QString js = QStringLiteral(R"JS((function () {
    var target = null;
    var outs = workspace.screens;
    for (var i = 0; i < outs.length; ++i) {
        if (outs[i].name === "%NAME%") {
            target = outs[i];
            break;
        }
    }
    var win = workspace.activeWindow;
    if (!target || !win || !win.moveableAcrossScreens) {
        return;
    }
    if (win.output && win.output.name === target.name) {
        return;
    }

    // Window::keepInArea(geometry, area) with partial == false: shrink to
    // fit, then clamp (right/bottom first, left/top last, like KWin).
    function clampInto(g, area) {
        var r = { x: g.x, y: g.y, width: g.width, height: g.height };
        if (r.width > area.width) {
            r.width = area.width;
        }
        if (r.height > area.height) {
            r.height = area.height;
        }
        if (r.x + r.width > area.x + area.width) {
            r.x = area.x + area.width - r.width;
        }
        if (r.y + r.height > area.y + area.height) {
            r.y = area.y + area.height - r.height;
        }
        if (r.x < area.x) {
            r.x = area.x;
        }
        if (r.y < area.y) {
            r.y = area.y;
        }
        return r;
    }

    // Window::moveToArea(): keep the position relative to the area center,
    // scaled between the two areas.
    function mapInto(g, oldArea, area) {
        var cx = (g.x + g.width / 2) - (oldArea.x + oldArea.width / 2);
        var cy = (g.y + g.height / 2) - (oldArea.y + oldArea.height / 2);
        cx = cx * area.width / oldArea.width + area.x + area.width / 2;
        cy = cy * area.height / oldArea.height + area.y + area.height / 2;
        return { x: cx - g.width / 2, y: cy - g.height / 2, width: g.width, height: g.height };
    }

    // Window::sendToOutput() geometry part for one window.
    function sendToOutput(win, target) {
        if (!win.output || win.output.name === target.name) {
            return;
        }
        var desktop = workspace.currentDesktopForScreen(win.output);
        var oldArea = workspace.clientArea("MaximizeArea", win.output, desktop);
        var newDesktop = workspace.currentDesktopForScreen(target);
        var newArea = workspace.clientArea("MaximizeArea", target, newDesktop);
        var g = win.frameGeometry;
        var mode = win.maximizeMode; // 0 restore, 1 vertical, 2 horizontal, 3 full
        var n;
        if (win.fullScreen) {
            var fs = workspace.clientArea("FullScreenArea", target, newDesktop);
            n = { x: fs.x, y: fs.y, width: fs.width, height: fs.height };
        } else {
            n = mapInto(g, oldArea, newArea);
            if (mode === 3) {
                n = { x: newArea.x, y: newArea.y, width: newArea.width, height: newArea.height };
            } else if (mode === 1 || mode === 2) {
                if (mode === 2) {
                    n.x = newArea.x;
                    n.width = newArea.width;
                }
                if (mode === 1) {
                    n.y = newArea.y;
                    n.height = newArea.height;
                }
                n = clampInto(n, newArea);
            } else if (oldArea.x <= g.x && g.x + g.width <= oldArea.x + oldArea.width
                && oldArea.y <= g.y && g.y + g.height <= oldArea.y + oldArea.height) {
                // moveToArea() only clamps when the window fit its old area.
                n = clampInto(n, newArea);
            }
        }
        win.frameGeometry = n;
    }

    sendToOutput(win, target);

    // Transients follow, like KWin's sendToOutput().
    var queue = [win];
    var windows = workspace.windowList();
    var done = [win];
    while (queue.length > 0) {
        var parent = queue.shift();
        for (var j = 0; j < windows.length; ++j) {
            var transient = windows[j];
            if (done.indexOf(transient) === -1 && transient.transientFor === parent) {
                done.push(transient);
                sendToOutput(transient, target);
                queue.push(transient);
            }
        }
    }
})();
)JS");
    QString escaped = outputName;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    js.replace(QStringLiteral("%NAME%"), escaped);
    return js;
}
} // namespace

namespace KWinScripts
{
bool isAvailable()
{
    // Wayland only: on X11 the scripting model's output names do not match
    // the panel's QScreen names, so matching by name would silently no-op.
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        return false;
    }
    return QDBusConnection::sessionBus().interface()->isServiceRegistered(QStringLiteral("org.kde.KWin"));
}

bool sendActiveWindowToOutput(const QString &outputName)
{
    if (outputName.isEmpty()) {
        return false;
    }

    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString directory = runtimeDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        : runtimeDir;
    if (directory.isEmpty()) {
        return false;
    }
    const QString scriptPath = directory + QLatin1Char('/') + QLatin1String(scriptFileName);

    {
        QSaveFile file(scriptPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning("QuickBar: cannot write the KWin bridge script to %s", qPrintable(scriptPath));
            return false;
        }
        file.write(buildScript(outputName).toUtf8());
        if (!file.commit()) {
            qWarning("QuickBar: cannot commit the KWin bridge script to %s", qPrintable(scriptPath));
            return false;
        }
    }

    QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"),
                             QDBusConnection::sessionBus());
    if (!scripting.isValid()) {
        return false;
    }

    // A script with this plugin name from a previous click is still loaded
    // (loadScript() refuses duplicate plugin names); replace it so the new
    // target takes effect, then run. Script::run() is idempotent, so other
    // already-running scripts are not re-executed.
    scripting.call(QStringLiteral("unloadScript"), QLatin1String(pluginName));
    const QDBusMessage loaded = scripting.call(QStringLiteral("loadScript"), scriptPath, QLatin1String(pluginName));
    if (loaded.type() != QDBusMessage::ReplyMessage) {
        qWarning("QuickBar: KWin rejected the bridge script: %s", qPrintable(loaded.errorMessage()));
        return false;
    }
    scripting.call(QStringLiteral("start"));
    return true;
}
}
