/*
    SPDX-FileCopyrightText: 2016 Kai Uwe Broulik <kde@privat.broulik.de>
    SPDX-FileCopyrightText: 2016 Chinmoy Ranjan Pradhan <chinmoyrp65@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "appmenumodel.h"
#include "kwinscripts.h"
#include "quickbarapplet.h"

#include "genericmenu.h"
#include "desktopmenu.h"
#include "menushortcutsender.h"
#include "quickbaractions.h"
#include "systemmenu.h"
#include "windowmenu.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QMenu>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>

// Includes for the menu search.
#include <KLocalizedString>
#include <QLineEdit>
#include <QListView>
#include <QWidgetAction>
#include <algorithm>
#include <csignal>
#include <unistd.h>

#include <QTimer>

#include <abstracttasksmodel.h>
#include <dbusmenuimporter.h>

class KDBusMenuImporter : public DBusMenuImporter
{
public:
    KDBusMenuImporter(const QString &service, const QString &path)
        : DBusMenuImporter(service, path)
    {
    }

protected:
    QIcon iconForName(const QString &name) override
    {
        return QIcon::fromTheme(name);
    }
};

namespace
{
QString visibleMenuText(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('&')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
                out += QLatin1Char('&');
                ++i;
            }
            continue;
        }
        out += ch;
    }
    return out;
}

void collectMenuActions(QMenu *menu, QList<QAction *> &result, QSet<QMenu *> &visited)
{
    if (!menu || visited.contains(menu)) {
        return;
    }
    visited.insert(menu);

    for (QAction *action : menu->actions()) {
        if (!action || action->isSeparator()) {
            continue;
        }
        if (QMenu *subMenu = action->menu()) {
            collectMenuActions(subMenu, result, visited);
        } else if (!action->text().isEmpty()) {
            result.append(action);
        }
    }
}
} // namespace

AppMenuModel::AppMenuModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_tasksModel(new TaskManager::TasksModel(this))
    , m_shortcutBridge(new MenuShortcutBridge(m_tasksModel, this))
    , m_serviceWatcher(new QDBusServiceWatcher(this))
{
    m_tasksModel->setFilterByScreen(!m_allScreens);
    connect(m_tasksModel, &TaskManager::TasksModel::activeTaskChanged, this, &AppMenuModel::onActiveWindowChanged);
    connect(m_tasksModel,
            &TaskManager::TasksModel::dataChanged,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles = QList<int>()) {
                Q_UNUSED(topLeft)
                Q_UNUSED(bottomRight)
                // The maximized flag is a single cheap data() read and only
                // notifies on change, so refresh it on every data change: the
                // backend may report a maximize toggle under roles other than
                // IsMaximized (e.g. geometry), and missing that update would
                // leave the hide-when-maximized option stuck.
                refreshActiveWindowMaximized();
                // Same for the application-name dropdown: capability and
                // maximize state can change without a window switch.
                updateWindowMenuState();
                if (roles.contains(TaskManager::AbstractTasksModel::ApplicationMenuObjectPath)
                    || roles.contains(TaskManager::AbstractTasksModel::ApplicationMenuServiceName)
                    || roles.contains(TaskManager::AbstractTasksModel::AppId)
                    || roles.contains(TaskManager::AbstractTasksModel::AppName)
                    || roles.contains(TaskManager::AbstractTasksModel::IsMaximized)
                    || roles.contains(Qt::DecorationRole)
                    || roles.isEmpty()) {
                    onActiveWindowChanged();
                }
            });
    // Model restructuring can remap the active task: keep the flag fresh.
    connect(m_tasksModel, &TaskManager::TasksModel::rowsInserted, this, &AppMenuModel::refreshActiveWindowMaximized);
    connect(m_tasksModel, &TaskManager::TasksModel::rowsRemoved, this, &AppMenuModel::refreshActiveWindowMaximized);
    connect(m_tasksModel, &TaskManager::TasksModel::modelReset, this, &AppMenuModel::refreshActiveWindowMaximized);
    connect(m_tasksModel, &TaskManager::TasksModel::layoutChanged, this, &AppMenuModel::refreshActiveWindowMaximized);
    connect(m_tasksModel, &TaskManager::TasksModel::activityChanged, this, &AppMenuModel::onActiveWindowChanged);
    connect(m_tasksModel, &TaskManager::TasksModel::virtualDesktopChanged, this, &AppMenuModel::onActiveWindowChanged);
    connect(m_tasksModel, &TaskManager::TasksModel::countChanged, this, &AppMenuModel::onActiveWindowChanged);
    connect(m_tasksModel, &TaskManager::TasksModel::screenGeometryChanged, this, &AppMenuModel::screenGeometryChanged);

    connect(this, &AppMenuModel::modelNeedsUpdate, this, [this] {
        if (!m_updatePending) {
            m_updatePending = true;
            QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
        }
    });

    onActiveWindowChanged();

    m_serviceWatcher->setConnection(QDBusConnection::sessionBus());
    // if our current DBus connection gets lost, close the menu
    // we'll select the new menu when the focus changes
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString &serviceName) {
        if (serviceName == m_serviceName) {
            clearApplicationMenu();
            onActiveWindowChanged();
        }
    });

    m_searchAction = new QAction(this);
    m_searchAction->setText(i18n("Search"));
    m_searchAction->setObjectName(QStringLiteral("appmenu"));

    m_searchMenu.reset(new QMenu);
    auto searchAction = new QWidgetAction(this);
    auto searchBar = new QLineEdit;
    searchBar->setClearButtonEnabled(true);
    searchBar->setPlaceholderText(i18n("Search…"));
    searchBar->setMinimumWidth(200);
    searchBar->setContentsMargins(4, 4, 4, 4);
    connect(m_tasksModel, &TaskManager::TasksModel::activeTaskChanged, searchBar, [searchBar]() {
        searchBar->setText(QString());
    });
    connect(searchBar, &QLineEdit::textChanged, this, [searchBar, this]() mutable {
        insertSearchActionsIntoMenu(searchBar->text());
    });
    connect(searchBar, &QLineEdit::returnPressed, this, [this]() mutable {
        if (!m_currentSearchActions.empty()) {
            m_currentSearchActions.constFirst()->trigger();
        }
    });
    connect(this, &AppMenuModel::modelNeedsUpdate, searchBar, [this, searchBar]() mutable {
        insertSearchActionsIntoMenu(searchBar->text());
    });
    connect(m_searchMenu.get(), &QMenu::aboutToShow, searchBar, [searchBar]() {
        QTimer::singleShot(0, searchBar, [searchBar]() {
            searchBar->setFocus(Qt::PopupFocusReason);
            searchBar->selectAll();
        });
    });
    searchAction->setDefaultWidget(searchBar);
    m_searchMenu->addAction(searchAction);
    m_searchMenu->addSeparator();
    m_searchAction->setMenu(m_searchMenu.get());
}

AppMenuModel::~AppMenuModel()
{
    removeSearchActionsFromMenu();
}

bool AppMenuModel::menuAvailable() const
{
    return m_menuAvailable;
}

void AppMenuModel::setMenuAvailable(bool set)
{
    if (m_menuAvailable != set) {
        m_menuAvailable = set;
        if (set) {
            setVisible(true);
        } else {
            setVisible(false);
        }
        Q_EMIT menuAvailableChanged();
        Q_EMIT menuForDisplayChanged();
    }
}

bool AppMenuModel::allScreens() const
{
    return m_allScreens;
}

void AppMenuModel::setallScreens(bool allScreens)
{
    if (m_allScreens == allScreens) {
        return;
    }

    m_allScreens = allScreens;
    m_tasksModel->setFilterByScreen(!m_allScreens);
    Q_EMIT allScreensChanged();
}

bool AppMenuModel::visible() const
{
    return m_visible;
}

bool AppMenuModel::activeWindowMaximized() const
{
    return m_activeWindowMaximized;
}

bool AppMenuModel::hasActiveWindow() const
{
    return m_hasActiveWindow;
}

void AppMenuModel::setHasActiveWindow(bool active)
{
    if (m_hasActiveWindow != active) {
        m_hasActiveWindow = active;
        Q_EMIT hasActiveWindowChanged();
    }
}

void AppMenuModel::setActiveWindowMaximized(bool maximized)
{
    if (m_activeWindowMaximized != maximized) {
        m_activeWindowMaximized = maximized;
        Q_EMIT activeWindowMaximizedChanged();
    }
}

bool AppMenuModel::isTaskMaximized(const QModelIndex &index) const
{
    if (!index.isValid() || !m_tasksModel) {
        return false;
    }
    if (m_tasksModel->data(index, TaskManager::AbstractTasksModel::IsGroupParent).toBool()) {
        const int kids = m_tasksModel->rowCount(index);
        for (int r = 0; r < kids; ++r) {
            const QModelIndex child = m_tasksModel->index(r, 0, index);
            if (m_tasksModel->data(child, TaskManager::AbstractTasksModel::IsActive).toBool()) {
                return isTaskMaximized(child);
            }
        }
        return false;
    }
    // Strictly the maximized state: fullscreen is a separate window state and
    // must not keep the flag set for ordinary windowed windows.
    return m_tasksModel->data(index, TaskManager::AbstractTasksModel::IsMaximized).toBool();
}

void AppMenuModel::refreshActiveWindowMaximized()
{
    if (!m_tasksModel) {
        setActiveWindowMaximized(false);
        return;
    }
    setActiveWindowMaximized(isTaskMaximized(m_tasksModel->activeTask()));
    // The backend can signal before its internal state settles (seen as the
    // flag sticking after unmaximize). One coalesced deferred re-read catches
    // the late value; it is a no-op when nothing changed.
    if (!m_maximizedRefreshQueued) {
        m_maximizedRefreshQueued = true;
        QTimer::singleShot(250, this, [this] {
            m_maximizedRefreshQueued = false;
            if (!m_tasksModel) {
                setActiveWindowMaximized(false);
                return;
            }
            setActiveWindowMaximized(isTaskMaximized(m_tasksModel->activeTask()));
        });
    }
}

bool AppMenuModel::menuForDisplay() const
{
    return m_menuAvailable;
}

bool AppMenuModel::stickyMenuBar() const
{
    return m_stickyMenuBar;
}

void AppMenuModel::setStickyMenuBar(bool sticky)
{
    if (m_stickyMenuBar == sticky) {
        return;
    }

    m_stickyMenuBar = sticky;
    Q_EMIT stickyMenuBarChanged();

    if (!sticky) {
        onActiveWindowChanged();
    } else {
        Q_EMIT menuForDisplayChanged();
    }
}

bool AppMenuModel::showDesktopMenu() const
{
    return m_showDesktopMenu;
}

void AppMenuModel::setShowDesktopMenu(bool show)
{
    if (m_showDesktopMenu == show) {
        return;
    }

    m_showDesktopMenu = show;
    Q_EMIT showDesktopMenuChanged();
    onActiveWindowChanged();
}

bool AppMenuModel::enableGenericMenu() const
{
    return m_enableGenericMenu;
}

void AppMenuModel::setEnableGenericMenu(bool enable)
{
    if (m_enableGenericMenu == enable) {
        return;
    }

    m_enableGenericMenu = enable;
    Q_EMIT enableGenericMenuChanged();
    onActiveWindowChanged();
}

bool AppMenuModel::enableMenuSearch() const
{
    return m_enableMenuSearch;
}

void AppMenuModel::setEnableMenuSearch(bool enable)
{
    if (m_enableMenuSearch == enable) {
        return;
    }

    m_enableMenuSearch = enable;
    Q_EMIT enableMenuSearchChanged();
    Q_EMIT modelNeedsUpdate();
}

QString AppMenuModel::applicationName() const
{
    return m_applicationName;
}

void AppMenuModel::setApplicationName(const QString &name)
{
    if (m_applicationName == name) {
        return;
    }

    m_applicationName = name;
    Q_EMIT applicationNameChanged();
}

QVariant AppMenuModel::applicationIcon() const
{
    return m_applicationIcon;
}

void AppMenuModel::setApplicationIcon(const QVariant &icon)
{
    if (m_applicationIcon == icon) {
        return;
    }

    m_applicationIcon = icon;
    Q_EMIT applicationIconChanged();
}

QString AppMenuModel::excludedItemsRegex() const
{
    return m_excludedItemsRegex;
}

void AppMenuModel::setExcludedItemsRegex(const QString &regex)
{
    if (m_excludedItemsRegex == regex) {
        return;
    }

    m_excludedItemsRegex = regex;
    m_compiledExcludedRegex = QRegularExpression(regex, QRegularExpression::CaseInsensitiveOption);
    Q_EMIT excludedItemsRegexChanged();
    Q_EMIT modelNeedsUpdate();
}

void AppMenuModel::setVisible(bool visible)
{
    if (m_visible != visible) {
        m_visible = visible;
        Q_EMIT visibleChanged();
        Q_EMIT menuForDisplayChanged();
    }
}

QRect AppMenuModel::screenGeometry() const
{
    return m_tasksModel->screenGeometry();
}

void AppMenuModel::setScreenGeometry(QRect geometry)
{
    m_tasksModel->setScreenGeometry(geometry);
}

bool AppMenuModel::includeSearchInModel() const
{
    return m_enableMenuSearch && m_searchAction;
}

QList<QAction *> AppMenuModel::visibleActions() const
{
    QList<QAction *> ret;
    if (!m_menuAvailable || !m_menu) {
        return ret;
    }
    const auto actions = m_menu->actions();
    for (QAction *action : actions) {
        if (!action) {
            continue;
        }
        if (!m_excludedItemsRegex.isEmpty() && m_compiledExcludedRegex.isValid()) {
            const QString rawText = action->text();
            const QString strippedText = visibleMenuText(rawText);
            if (m_compiledExcludedRegex.match(rawText).hasMatch() || m_compiledExcludedRegex.match(strippedText).hasMatch()) {
                continue;
            }
        }
        ret.append(action);
    }
    return ret;
}

int AppMenuModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    if (!m_menuAvailable || !m_menu) {
        return 0;
    }

    const auto actions = visibleActions();
    const int actionCount = actions.count();
    // Avoid showing only "Search" while the DBus menu is still loading or temporarily empty.
    if (actionCount == 0) {
        return 0;
    }

    return actionCount + (includeSearchInModel() ? 1 : 0);
}

void AppMenuModel::removeSearchActionsFromMenu()
{
    if (!m_searchAction || !m_searchAction->menu()) {
        m_currentSearchActions.clear();
        return;
    }
    for (auto action : std::as_const(m_currentSearchActions)) {
        if (action) {
            m_searchAction->menu()->removeAction(action);
        }
    }
    m_currentSearchActions.clear();
}

void AppMenuModel::insertSearchActionsIntoMenu(const QString &filter)
{
    removeSearchActionsFromMenu();
    if (filter.isEmpty() || !m_searchAction || !m_searchAction->menu()) {
        return;
    }
    const auto actions = flatActionList();
    for (const auto &action : actions) {
        if (action->text().contains(filter, Qt::CaseInsensitive)
            || visibleMenuText(action->text()).contains(filter, Qt::CaseInsensitive)) {
            m_searchAction->menu()->addAction(action);
            m_currentSearchActions << action;
        }
    }
}

void AppMenuModel::update()
{
    beginResetModel();
    endResetModel();
    m_updatePending = false;
}

void AppMenuModel::onActiveWindowChanged()
{
    QuickBarApplet::ensureServiceRegistered();

    // Do not change active window when panel gets focus
    // See ShellCorona::init() in shell/shellcorona.cpp
    if (m_containmentStatus == Plasma::Types::AcceptingInputStatus) {
        return;
    }

    auto updateAppNameAndIcon = [this](const QModelIndex &index) {
        if (index.isValid()) {
            QString name = m_tasksModel->data(index, TaskManager::AbstractTasksModel::AppName).toString();
            if (name.isEmpty()) {
                name = m_tasksModel->data(index, TaskManager::AbstractTasksModel::GenericName).toString();
            }
            if (name.isEmpty()) {
                QString appId = m_tasksModel->data(index, TaskManager::AbstractTasksModel::AppId).toString();
                if (!appId.isEmpty()) {
                    if (appId.endsWith(QLatin1String(".desktop"))) {
                        appId.chop(8);
                    }
                    const int lastDot = appId.lastIndexOf(QLatin1Char('.'));
                    if (lastDot >= 0 && lastDot + 1 < appId.length()) {
                        name = appId.mid(lastDot + 1);
                    } else {
                        name = appId;
                    }
                }
            }
            if (name.isEmpty()) {
                name = m_tasksModel->data(index, Qt::DisplayRole).toString();
            }
            setApplicationName(name);
            QVariant iconVar = m_tasksModel->data(index, Qt::DecorationRole);
            if (!iconVar.isValid() || iconVar.isNull()) {
                iconVar = m_tasksModel->data(index, TaskManager::AbstractTasksModel::AppId);
            }
            setApplicationIcon(iconVar);
        } else if (m_showDesktopMenu) {
            setApplicationName(i18n("Desktop"));
            setApplicationIcon(QStringLiteral("plasma"));
        } else {
            setApplicationName(QString());
            setApplicationIcon(QVariant());
        }
    };

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    updateAppNameAndIcon(activeTaskIndex);
    refreshActiveWindowMaximized();
    setHasActiveWindow(activeTaskIndex.isValid());
    updateWindowMenuState();

    if (activeTaskIndex.isValid()) {
        const QString objectPath = m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::ApplicationMenuObjectPath).toString();
        const QString serviceName = m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::ApplicationMenuServiceName).toString();

        if (!objectPath.isEmpty() && !serviceName.isEmpty()) {
            m_usingGenericMenu = false;
            updateApplicationMenu(serviceName, objectPath);
            return;
        }

        if (shouldUseGenericMenu(activeTaskIndex)) {
            applyGenericMenu();
            return;
        }
    } else if (m_showDesktopMenu) {
        applyDesktopMenu();
        return;
    }

    clearApplicationMenu();
}

bool AppMenuModel::shouldUseGenericMenu(const QModelIndex &activeTaskIndex) const
{
    if (!m_enableGenericMenu) {
        return false;
    }
    if (!activeTaskIndex.isValid()) {
        // The desktop shows its own Places menu; the generic menu is
        // only a fallback for application windows without a menu.
        return false;
    }
    return true;
}

void AppMenuModel::clearApplicationMenu()
{
    m_usingGenericMenu = false;
    m_usingDesktopMenu = false;
    setMenuAvailable(false);
    setVisible(false);

    m_serviceName.clear();
    m_menuObjectPath.clear();
    m_serviceWatcher->setWatchedServices({});
    m_importer.reset();
    m_menu.clear();
    Q_EMIT modelNeedsUpdate();
}

void AppMenuModel::quitActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()
        || !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsClosable).toBool()) {
        return;
    }

    m_tasksModel->requestClose(activeTaskIndex);
}

void AppMenuModel::minimizeActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()
        || !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMinimizable).toBool()
        || m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMinimized).toBool()) {
        return;
    }

    m_tasksModel->requestToggleMinimized(activeTaskIndex);
}

void AppMenuModel::maximizeActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()
        || !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMaximizable).toBool()) {
        return;
    }

    m_tasksModel->requestToggleMaximized(activeTaskIndex);
}

void AppMenuModel::keepAboveActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (activeTaskIndex.isValid()) {
        m_tasksModel->requestToggleKeepAbove(activeTaskIndex);
    }
}

void AppMenuModel::keepBelowActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (activeTaskIndex.isValid()) {
        m_tasksModel->requestToggleKeepBelow(activeTaskIndex);
    }
}

void AppMenuModel::fullScreenActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()
        || !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsFullScreenable).toBool()) {
        return;
    }

    m_tasksModel->requestToggleFullScreen(activeTaskIndex);
}

void AppMenuModel::noBorderActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()
        || !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::CanSetNoBorder).toBool()) {
        return;
    }

    m_tasksModel->requestToggleNoBorder(activeTaskIndex);
}

void AppMenuModel::excludeFromCaptureActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()) {
        return;
    }

    m_tasksModel->requestToggleExcludeFromCapture(activeTaskIndex);
}

void AppMenuModel::moveActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()
        || !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMovable).toBool()) {
        return;
    }

    m_tasksModel->requestMove(activeTaskIndex);
}

void AppMenuModel::resizeActiveTask()
{
    if (!m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    if (!activeTaskIndex.isValid()
        || !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsResizable).toBool()) {
        return;
    }

    m_tasksModel->requestResize(activeTaskIndex);
}

void AppMenuModel::openWindowRules(bool wholeApplication)
{
    const QString kcmshell = QStandardPaths::findExecutable(QStringLiteral("kcmshell6"));
    if (kcmshell.isEmpty()) {
        return;
    }

    // KCMRules parses bare "uuid=<value>" and "whole-app" tokens from the
    // module arguments (kcmshell --args). On Wayland the task manager's
    // window id is the KWin window uuid the KCM expects; on X11 it is a
    // numeric WId, which the KCM does not accept, so it opens unfiltered
    // and offers its own window detection there.
    QStringList moduleArgs;
    const QModelIndex activeTaskIndex = m_tasksModel ? m_tasksModel->activeTask() : QModelIndex();
    if (activeTaskIndex.isValid()) {
        const QVariantList winIds = m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::WinIdList).toList();
        if (!winIds.isEmpty()) {
            const QUuid uuid(winIds.first().toString());
            if (!uuid.isNull()) {
                moduleArgs << QStringLiteral("uuid=") + uuid.toString();
            }
        }
    }

    if (wholeApplication) {
        moduleArgs << QStringLiteral("whole-app");
    }

    QStringList arguments{QStringLiteral("kcm_kwinrules")};
    if (!moduleArgs.isEmpty()) {
        arguments << QStringLiteral("--args") << moduleArgs.join(QLatin1Char(' '));
    }

    QProcess::startDetached(kcmshell, arguments);
}

namespace
{
// Launch the Plasma logout prompt for the given method (promptLogout,
// promptReboot or promptShutDown): the standard confirmation dialog used by
// Kickoff, so QuickBar never needs privileges for session actions.
void callLogoutPrompt(QLatin1StringView method)
{
    QDBusInterface prompt(QStringLiteral("org.kde.LogoutPrompt"), QStringLiteral("/LogoutPrompt"),
                          QStringLiteral("org.kde.LogoutPrompt"), QDBusConnection::sessionBus());
    prompt.call(method);
}
} // namespace

void AppMenuModel::openAboutSystem()
{
    const QString kinfocenter = QStandardPaths::findExecutable(QStringLiteral("kinfocenter"));
    if (!kinfocenter.isEmpty()) {
        QProcess::startDetached(kinfocenter, QStringList());
    }
}

void AppMenuModel::openSystemSettings()
{
    const QString systemSettings = QStandardPaths::findExecutable(QStringLiteral("systemsettings"));
    if (!systemSettings.isEmpty()) {
        QProcess::startDetached(systemSettings, QStringList());
    }
}

void AppMenuModel::lockScreen()
{
    QDBusInterface screensaver(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"),
                              QStringLiteral("org.freedesktop.ScreenSaver"), QDBusConnection::sessionBus());
    screensaver.call(QStringLiteral("Lock"));
}

void AppMenuModel::logoutSession()
{
    callLogoutPrompt(QLatin1StringView("promptLogout"));
}

void AppMenuModel::sleepSession()
{
    // powerdevil's freedesktop PowerManagement Suspend() runs the
    // SuspendSession action with the user's profile sleep mode (powerdevilrc
    // SleepMode: suspend-to-ram, hybrid sleep or suspend-then-hibernate) —
    // exactly what their power button does — and honors session inhibits.
    // Without powerdevil, fall back to logind's plain suspend.
    QDBusInterface fdo(QStringLiteral("org.freedesktop.PowerManagement"),
                       QStringLiteral("/org/freedesktop/PowerManagement"),
                       QStringLiteral("org.freedesktop.PowerManagement"),
                       QDBusConnection::sessionBus());
    if (fdo.isValid()) {
        fdo.call(QStringLiteral("Suspend"));
        return;
    }
    QDBusInterface login1(QStringLiteral("org.freedesktop.login1"),
                          QStringLiteral("/org/freedesktop/login1"),
                          QStringLiteral("org.freedesktop.login1.Manager"),
                          QDBusConnection::systemBus());
    login1.call(QStringLiteral("Suspend"), false);
}

void AppMenuModel::hibernateSession()
{
    // Canonical hibernate through powerdevil first (polkit and inhibit
    // handling, capability-checked); fall back to logind when powerdevil is
    // not running: plain hibernate when logind allows it, otherwise
    // suspend-then-hibernate as the only hibernating flow some systems
    // permit (sleep.conf Allow flags).
    QDBusInterface fdo(QStringLiteral("org.freedesktop.PowerManagement"),
                       QStringLiteral("/org/freedesktop/PowerManagement"),
                       QStringLiteral("org.freedesktop.PowerManagement"),
                       QDBusConnection::sessionBus());
    if (fdo.isValid()) {
        const QDBusMessage canHibernate = fdo.call(QStringLiteral("CanHibernate"));
        if (canHibernate.type() == QDBusMessage::ReplyMessage
            && canHibernate.arguments().value(0).toBool()) {
            fdo.call(QStringLiteral("Hibernate"));
            return;
        }
    }

    QDBusInterface login1(QStringLiteral("org.freedesktop.login1"),
                          QStringLiteral("/org/freedesktop/login1"),
                          QStringLiteral("org.freedesktop.login1.Manager"),
                          QDBusConnection::systemBus());
    if (login1.call(QStringLiteral("CanHibernate")).arguments().value(0).toString() != QLatin1String("no")) {
        login1.call(QStringLiteral("Hibernate"), false);
    } else if (login1.call(QStringLiteral("CanSuspendThenHibernate")).arguments().value(0).toString() != QLatin1String("no")) {
        login1.call(QStringLiteral("SuspendThenHibernate"), false);
    }
}

bool AppMenuModel::powerDevilCanDo(const char *method)
{
    QDBusInterface fdo(QStringLiteral("org.freedesktop.PowerManagement"),
                       QStringLiteral("/org/freedesktop/PowerManagement"),
                       QStringLiteral("org.freedesktop.PowerManagement"),
                       QDBusConnection::sessionBus());
    if (!fdo.isValid()) {
        return false;
    }
    const QDBusMessage reply = fdo.call(QLatin1String(method));
    return reply.type() == QDBusMessage::ReplyMessage && reply.arguments().value(0).toBool();
}

bool AppMenuModel::logindAllows(const char *method)
{
    QDBusInterface login1(QStringLiteral("org.freedesktop.login1"),
                          QStringLiteral("/org/freedesktop/login1"),
                          QStringLiteral("org.freedesktop.login1.Manager"),
                          QDBusConnection::systemBus());
    return login1.call(QLatin1String(method)).arguments().value(0).toString() != QLatin1String("no");
}

bool AppMenuModel::sleepAvailable()
{
    return powerDevilCanDo("CanSuspend") || powerDevilCanDo("CanSuspendThenHibernate")
        || logindAllows("CanSuspend");
}

bool AppMenuModel::hibernateAvailable()
{
    return powerDevilCanDo("CanHibernate") || logindAllows("CanHibernate")
        || logindAllows("CanSuspendThenHibernate");
}

void AppMenuModel::restartSession()
{
    callLogoutPrompt(QLatin1StringView("promptReboot"));
}

void AppMenuModel::shutdownSession()
{
    callLogoutPrompt(QLatin1StringView("promptShutDown"));
}

void AppMenuModel::wireMenuActions(QMenu *menu)
{
    if (!menu) {
        return;
    }

    const auto actions = menu->findChildren<QAction *>();
    for (QAction *action : actions) {
        const QString actionId = action->property(QuickBarActions::property).toString();
        if (actionId == QLatin1String(QuickBarActions::quit)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::quitActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::minimize)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::minimizeActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::maximize)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::maximizeActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::keepAbove)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::keepAboveActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::keepBelow)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::keepBelowActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::fullScreen)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::fullScreenActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::noBorder)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::noBorderActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::excludeFromCapture)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::excludeFromCaptureActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::move)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::moveActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::resize)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::resizeActiveTask);
        } else if (actionId == QLatin1String(QuickBarActions::windowSettings)) {
            connect(action, &QAction::triggered, this, [this] { openWindowRules(false); });
        } else if (actionId == QLatin1String(QuickBarActions::appSettings)) {
            connect(action, &QAction::triggered, this, [this] { openWindowRules(true); });
        } else if (actionId == QLatin1String(QuickBarActions::about)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::requestOpenAbout);
        } else if (actionId == QLatin1String(QuickBarActions::aboutSystem)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::openAboutSystem);
        } else if (actionId == QLatin1String(QuickBarActions::systemSettings)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::openSystemSettings);
        } else if (actionId == QLatin1String(QuickBarActions::lockScreen)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::lockScreen);
        } else if (actionId == QLatin1String(QuickBarActions::logout)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::logoutSession);
        } else if (actionId == QLatin1String(QuickBarActions::sleep)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::sleepSession);
        } else if (actionId == QLatin1String(QuickBarActions::hibernate)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::hibernateSession);
        } else if (actionId == QLatin1String(QuickBarActions::restart)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::restartSession);
        } else if (actionId == QLatin1String(QuickBarActions::shutdown)) {
            connect(action, &QAction::triggered, this, &AppMenuModel::shutdownSession);
        }
    }
}

void AppMenuModel::updateMenuActionState(QMenu *menu)
{
    if (!menu || !m_tasksModel) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    const bool canClose = activeTaskIndex.isValid()
        && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsClosable).toBool();
    const bool canMinimize = activeTaskIndex.isValid()
        && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMinimizable).toBool()
        && !m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMinimized).toBool();
    const bool canMaximize = activeTaskIndex.isValid()
        && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMaximizable).toBool();
    const bool maximized = isTaskMaximized(activeTaskIndex);

    const auto actions = menu->findChildren<QAction *>();
    for (QAction *action : actions) {
        const QString actionId = action->property(QuickBarActions::property).toString();
        if (actionId == QLatin1String(QuickBarActions::quit)) {
            action->setEnabled(canClose);
        } else if (actionId == QLatin1String(QuickBarActions::minimize)) {
            action->setEnabled(canMinimize);
        } else if (actionId == QLatin1String(QuickBarActions::maximize)) {
            action->setEnabled(canMaximize);
            // Toggled state is shown with the checkmark, exactly like
            // KWin's window operations menu.
            action->setChecked(maximized);
        } else if (actionId == QLatin1String(QuickBarActions::move)) {
            action->setEnabled(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMovable).toBool());
        } else if (actionId == QLatin1String(QuickBarActions::resize)) {
            action->setEnabled(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsResizable).toBool());
        } else if (actionId == QLatin1String(QuickBarActions::keepAbove)) {
            action->setChecked(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsKeepAbove).toBool());
        } else if (actionId == QLatin1String(QuickBarActions::keepBelow)) {
            action->setChecked(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsKeepBelow).toBool());
        } else if (actionId == QLatin1String(QuickBarActions::fullScreen)) {
            action->setEnabled(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsFullScreenable).toBool());
            action->setChecked(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsFullScreen).toBool());
        } else if (actionId == QLatin1String(QuickBarActions::noBorder)) {
            action->setEnabled(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::CanSetNoBorder).toBool());
            action->setChecked(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::HasNoBorder).toBool());
        } else if (actionId == QLatin1String(QuickBarActions::excludeFromCapture)) {
            // Wayland-only capability: hidden entirely on X11, like KWin.
            const bool wayland = QGuiApplication::platformName() == QStringLiteral("wayland");
            action->setVisible(wayland);
            action->setEnabled(wayland && activeTaskIndex.isValid());
            action->setChecked(activeTaskIndex.isValid()
                && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsExcludedFromCapture).toBool());
        }
    }
}

QMenu *AppMenuModel::windowMenu()
{
    ensureWindowMenuCreated();
    return m_windowMenu.get();
}

QMenu *AppMenuModel::systemMenu()
{
    ensureSystemMenuCreated();
    updateSystemMenuState();
    return m_systemMenu.get();
}

void AppMenuModel::ensureSystemMenuCreated()
{
    if (!m_systemMenu) {
        m_systemMenu.reset(SystemMenu::create());
        wireMenuActions(m_systemMenu.get());
        const auto actions = m_systemMenu->findChildren<QAction *>();
        for (QAction *action : actions) {
            const QString actionId = action->property(QuickBarActions::property).toString();
            if (actionId == QLatin1String(QuickBarActions::sleep)) {
                m_sleepAction = action;
            } else if (actionId == QLatin1String(QuickBarActions::hibernate)) {
                m_hibernateAction = action;
            }
        }
    }
}

void AppMenuModel::updateSystemMenuState()
{
    // Availability is re-checked on every open: the capability answers
    // depend on powerdevil/logind state (sleep.conf Allow flags, swap
    // layout, running services), not on anything static.
    if (m_sleepAction) {
        m_sleepAction->setVisible(sleepAvailable());
    }
    if (m_hibernateAction) {
        m_hibernateAction->setVisible(hibernateAvailable());
    }
}

void AppMenuModel::ensureWindowMenuCreated()
{
    if (!m_windowMenu) {
        m_windowMenu.reset(WindowMenu::create());
        wireMenuActions(m_windowMenu.get());
        // Dynamic submenus, rebuilt for the current task whenever the window
        // menu state refreshes or the submenus are about to open (fresh
        // desktop/screen lists, like KWin's own popupAboutToShow rebuilds).
        m_desktopsMenu = m_windowMenu->findChild<QMenu *>(QStringLiteral("quickbar-desktops-menu"));
        m_screensMenu = m_windowMenu->findChild<QMenu *>(QStringLiteral("quickbar-screens-menu"));
        if (m_desktopsMenu) {
            connect(m_desktopsMenu.data(), &QMenu::aboutToShow, this, &AppMenuModel::rebuildDesktopsMenu, Qt::UniqueConnection);
        }
        if (m_screensMenu) {
            connect(m_screensMenu.data(), &QMenu::aboutToShow, this, &AppMenuModel::rebuildScreensMenu, Qt::UniqueConnection);
        }
        if (!m_virtualDesktopInfo) {
            m_virtualDesktopInfo = new TaskManager::VirtualDesktopInfo(this);
        }
    }
    updateMenuActionState(m_windowMenu.get());
    rebuildDesktopsMenu();
    rebuildScreensMenu();
}

void AppMenuModel::updateWindowMenuState()
{
    if (m_windowMenu) {
        updateMenuActionState(m_windowMenu.get());
        rebuildDesktopsMenu();
        rebuildScreensMenu();
    }
}

void AppMenuModel::rebuildDesktopsMenu()
{
    if (!m_windowMenu || !m_desktopsMenu || !m_tasksModel || !m_virtualDesktopInfo) {
        return;
    }

    const QModelIndex activeTaskIndex = m_tasksModel->activeTask();
    const bool changeable = activeTaskIndex.isValid()
        && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsVirtualDesktopsChangeable).toBool();
    const int desktopCount = m_virtualDesktopInfo->numberOfDesktops();
    // KWin only shows the desktops menu with more than one desktop.
    m_desktopsMenu->menuAction()->setVisible(desktopCount > 1 && changeable);
    if (desktopCount <= 1 || !changeable) {
        return;
    }

    m_desktopsMenu->clear();

    auto activeTask = [this]() -> QModelIndex {
        return m_tasksModel ? m_tasksModel->activeTask() : QModelIndex();
    };

    // Empty list means "on all virtual desktops"; unchecking moves the
    // window to the current desktop of its own screen.
    auto *allDesktops = m_desktopsMenu->addAction(i18n("&All Desktops"));
    allDesktops->setCheckable(true);
    allDesktops->setChecked(activeTaskIndex.isValid()
        && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsOnAllVirtualDesktops).toBool());
    connect(allDesktops, &QAction::triggered, this, [this, activeTask] {
        const QModelIndex idx = activeTask();
        if (!idx.isValid()) {
            return;
        }
        if (m_tasksModel->data(idx, TaskManager::AbstractTasksModel::IsOnAllVirtualDesktops).toBool()) {
            const QRect geometry = m_tasksModel->data(idx, TaskManager::AbstractTasksModel::ScreenGeometry).toRect();
            const QVariant current = m_virtualDesktopInfo->currentDesktopByScreenGeometry(geometry);
            m_tasksModel->requestVirtualDesktops(idx, QVariantList{current});
        } else {
            m_tasksModel->requestVirtualDesktops(idx, QVariantList());
        }
    });

    m_desktopsMenu->addSeparator();

    const QVariantList desktopIds = m_virtualDesktopInfo->desktopIds();
    const QStringList desktopNames = m_virtualDesktopInfo->desktopNames();
    const QVariantList taskDesktops = activeTaskIndex.isValid()
        ? m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::VirtualDesktops).toList()
        : QVariantList();
    const bool onAllDesktops = activeTaskIndex.isValid()
        && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsOnAllVirtualDesktops).toBool();
    for (int i = 0; i < desktopIds.count(); ++i) {
        const QVariant id = desktopIds.at(i);
        auto *desktopAction = m_desktopsMenu->addAction(desktopNames.value(i).replace(QLatin1Char('&'), QStringLiteral("&&")));
        desktopAction->setCheckable(true);
        // Like KWin: unchecked while the window is on all desktops.
        desktopAction->setChecked(!onAllDesktops && taskDesktops.contains(id));
        connect(desktopAction, &QAction::triggered, this, [this, id, activeTask] {
            const QModelIndex idx = activeTask();
            if (!idx.isValid()) {
                return;
            }
            // Toggling adds/removes the desktop, leaving the window on
            // several desktops simultaneously, like KWin's add/remove.
            QVariantList current = m_tasksModel->data(idx, TaskManager::AbstractTasksModel::VirtualDesktops).toList();
            if (m_tasksModel->data(idx, TaskManager::AbstractTasksModel::IsOnAllVirtualDesktops).toBool()) {
                // "All" is the empty list; resolve it explicitly, then
                // remove the clicked desktop (KWin's semantics for
                // toggling off one desktop of an all-desktops window).
                current = m_virtualDesktopInfo->desktopIds();
            }
            if (current.contains(id)) {
                current.removeAll(id);
            } else {
                current.append(id);
            }
            m_tasksModel->requestVirtualDesktops(idx, current);
        });
    }

    m_desktopsMenu->addSeparator();

    // Plain (non-checkable) exclusive-move entries, like KWin's.
    for (int i = 0; i < desktopIds.count(); ++i) {
        const QVariant id = desktopIds.at(i);
        auto *moveAction = m_desktopsMenu->addAction(i18n("Move to %1", desktopNames.value(i)));
        connect(moveAction, &QAction::triggered, this, [this, id, activeTask] {
            const QModelIndex idx = activeTask();
            if (idx.isValid()) {
                m_tasksModel->requestVirtualDesktops(idx, QVariantList{id});
            }
        });
    }

    m_desktopsMenu->addSeparator();

    // Creates a desktop and keeps the window on its current desktops too
    // (KWin's "Add to New Desktop"). requestNewVirtualDesktop moves the
    // window exclusively; once the new desktop id is known, the previous
    // desktops are restored on top of it.
    auto *addNewDesktop = m_desktopsMenu->addAction(
        i18nc("Create a new desktop and add the window to that desktop", "Add to &New Desktop"));
    connect(addNewDesktop, &QAction::triggered, this, [this, activeTask] {
        const QPersistentModelIndex idx(activeTask());
        if (!idx.isValid() || !m_virtualDesktopInfo) {
            return;
        }
        QVariantList current = m_tasksModel->data(idx, TaskManager::AbstractTasksModel::VirtualDesktops).toList();
        if (m_tasksModel->data(idx, TaskManager::AbstractTasksModel::IsOnAllVirtualDesktops).toBool()) {
            current = m_virtualDesktopInfo->desktopIds();
        }
        const QVariantList oldIds = m_virtualDesktopInfo->desktopIds();
        m_tasksModel->requestNewVirtualDesktop(idx);
        connect(m_virtualDesktopInfo, &TaskManager::VirtualDesktopInfo::desktopIdsChanged, this,
                [this, idx, current, oldIds] {
                    const QVariantList newIds = m_virtualDesktopInfo->desktopIds();
                    QVariantList created;
                    for (const QVariant &id : newIds) {
                        if (!oldIds.contains(id)) {
                            created.append(id);
                        }
                    }
                    if (!created.isEmpty() && idx.isValid()) {
                        QVariantList desktops = current;
                        desktops.append(created.last());
                        m_tasksModel->requestVirtualDesktops(idx, desktops);
                    }
                },
                Qt::SingleShotConnection);
    });

    // Creates a desktop and enters the window on it exclusively (KWin's
    // "Move to New Desktop").
    auto *newDesktop = m_desktopsMenu->addAction(
        i18nc("Create a new desktop and move the window to that desktop", "Move to New Desktop"));
    connect(newDesktop, &QAction::triggered, this, [this, activeTask] {
        const QModelIndex idx = activeTask();
        if (idx.isValid()) {
            m_tasksModel->requestNewVirtualDesktop(idx);
        }
    });
}

void AppMenuModel::rebuildScreensMenu()
{
    if (!m_windowMenu || !m_screensMenu) {
        return;
    }

    // TasksModel::requestSendToOutput only exists since Plasma 6.9; discover
    // it at runtime. On older stacks fall back to a one-shot KWin scripting
    // bridge (KWinScripts) that mirrors KWin's own "Move to Screen"
    // behavior, so the entry works today and switches to the native request
    // automatically once the task manager grows the method.
    const bool nativeSupported = m_tasksModel
        && m_tasksModel->metaObject()->indexOfMethod(QByteArrayLiteral("requestSendToOutput(QModelIndex,QScreen*)")) != -1;
    const bool supported = nativeSupported || KWinScripts::isAvailable();
    const QList<QScreen *> screens = QGuiApplication::screens();
    // KWin only shows the screen menu with more than one output and for
    // movable windows.
    const QModelIndex activeTaskIndex = m_tasksModel ? m_tasksModel->activeTask() : QModelIndex();
    const bool movable = activeTaskIndex.isValid()
        && m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::IsMovable).toBool();
    m_screensMenu->menuAction()->setVisible(supported && screens.count() > 1 && movable);
    if (!supported || screens.count() <= 1 || !movable) {
        return;
    }

    m_screensMenu->clear();

    const QRect taskGeometry = activeTaskIndex.isValid()
        ? m_tasksModel->data(activeTaskIndex, TaskManager::AbstractTasksModel::ScreenGeometry).toRect()
        : QRect();
    for (int i = 0; i < screens.count(); ++i) {
        QScreen *screen = screens.at(i);
        auto *screenAction = m_screensMenu->addAction(
            i18nc("@item:inmenu List of all Screens to send a window to. First argument is a number, second the output identifier. E.g. Screen 1 (HDMI1)",
                  "Screen &%1 (%2)", (i + 1), screen->name()));
        screenAction->setCheckable(true);
        screenAction->setChecked(taskGeometry == screen->geometry());
        connect(screenAction, &QAction::triggered, this, [this, screen, nativeSupported] {
            const QModelIndex idx = m_tasksModel ? m_tasksModel->activeTask() : QModelIndex();
            if (!idx.isValid()) {
                return;
            }
            if (nativeSupported) {
                QMetaObject::invokeMethod(m_tasksModel, "requestSendToOutput",
                                          Q_ARG(QModelIndex, idx), Q_ARG(QScreen *, screen));
            } else {
                KWinScripts::sendActiveWindowToOutput(screen->name());
            }
        });
    }
}

bool AppMenuModel::genericMenuHasDuplicateActions(const QMenu *menu) const
{
    if (!menu) {
        return false;
    }

    int aboutCount = 0;
    const auto actions = menu->findChildren<QAction *>();
    for (const QAction *action : actions) {
        if (action->property(QuickBarActions::property).toString() == QLatin1String(QuickBarActions::about)) {
            if (++aboutCount > 1) {
                return true;
            }
        }
    }
    return false;
}

void AppMenuModel::ensureGenericMenuCreated()
{
    if (m_genericMenu && genericMenuHasDuplicateActions(m_genericMenu.get())) {
        // Avoid deleting the generic menu while a proxy clone might be visible.
        // The proxy's clones are connected to the source actions; deleting the source
        // while the proxy is popup-visible would disconnect the clones but is still
        // racy if the menu is being torn down during QMenu::popup's window creation.
        // Defer the reset if we're currently displaying the generic menu.
        if (m_usingGenericMenu && m_menu == m_genericMenu.get() && m_menuAvailable) {
            // Schedule a safe reset for the next event loop and reuse current menu for now.
            QTimer::singleShot(0, this, [this]() {
                if (m_genericMenu && genericMenuHasDuplicateActions(m_genericMenu.get())) {
                    m_genericMenu.reset();
                    if (m_usingGenericMenu) {
                        QMetaObject::invokeMethod(this, [this]() { applyGenericMenu(); }, Qt::QueuedConnection);
                    }
                }
            });
        } else {
            m_genericMenu.reset();
        }
    }

    if (!m_genericMenu) {
        m_genericMenu.reset(GenericMenu::create());

        m_shortcutBridge->wireMenu(m_genericMenu.get());
        wireMenuActions(m_genericMenu.get());

        const auto actions = m_genericMenu->actions();
        for (QAction *action : actions) {
            connect(action, &QAction::changed, this, [this, action] {
                if (!m_menuAvailable || !m_menu) {
                    return;
                }
                const int actionIdx = m_menu->actions().indexOf(action);
                if (actionIdx > -1) {
                    const QModelIndex modelIdx = index(actionIdx, 0);
                    Q_EMIT dataChanged(modelIdx, modelIdx);
                }
            });
            connect(action, &QAction::destroyed, this, &AppMenuModel::modelNeedsUpdate, Qt::UniqueConnection);
        }
    }

    updateMenuActionState(m_genericMenu.get());
}

void AppMenuModel::applyGenericMenu()
{
    ensureGenericMenuCreated();

    if (m_usingGenericMenu && m_menu == m_genericMenu.get() && m_menuAvailable) {
        setVisible(true);
        return;
    }

    m_usingGenericMenu = true;
    m_serviceName.clear();
    m_menuObjectPath.clear();
    m_serviceWatcher->setWatchedServices({});
    m_importer.reset();

    m_menu = m_genericMenu.get();

    setMenuAvailable(true);
    setVisible(true);
    Q_EMIT modelNeedsUpdate();
}

void AppMenuModel::ensureDesktopMenuCreated()
{
    if (!m_desktopMenu) {
        m_desktopMenu = std::make_unique<DesktopMenu>(this);
    }
}

void AppMenuModel::applyDesktopMenu()
{
    ensureDesktopMenuCreated();
    // Places may have changed while the dropdown was borrowed by an open
    // popup; pick up any deferred rebuild now.
    m_desktopMenu->flushPendingRebuild();

    if (m_usingDesktopMenu && m_menu == m_desktopMenu->menu() && m_menuAvailable) {
        setVisible(true);
        return;
    }

    m_usingGenericMenu = false;
    m_usingDesktopMenu = true;
    m_serviceName.clear();
    m_menuObjectPath.clear();
    m_serviceWatcher->setWatchedServices({});
    m_importer.reset();

    m_menu = m_desktopMenu->menu();

    setMenuAvailable(true);
    setVisible(true);
    Q_EMIT modelNeedsUpdate();
}

QHash<int, QByteArray> AppMenuModel::roleNames() const
{
    QHash<int, QByteArray> roleNames;
    roleNames[MenuRole] = QByteArrayLiteral("activeMenu");
    roleNames[ActionRole] = QByteArrayLiteral("activeActions");
    return roleNames;
}

QList<QAction *> AppMenuModel::flatActionList()
{
    QList<QAction *> ret;
    if (!m_menuAvailable || !m_menu) {
        return ret;
    }
    QSet<QMenu *> visited;
    collectMenuActions(m_menu.data(), ret, visited);
    return ret;
}

QVariant AppMenuModel::data(const QModelIndex &index, int role) const
{
    if (!m_menuAvailable || !m_menu) {
        return {};
    }

    if (!index.isValid()) {
        if (role == MenuRole) {
            return QString();
        } else if (role == ActionRole) {
            return QVariant::fromValue(m_menu->menuAction());
        }
    }

    const auto actions = visibleActions();
    if (actions.isEmpty()) {
        return {};
    }

    const int row = index.row();
    if (row == actions.count() && includeSearchInModel()) {
        if (role == MenuRole) {
            return m_searchAction->text();
        } else if (role == ActionRole) {
            return QVariant::fromValue(m_searchAction.data());
        }
    }
    if (row >= actions.count()) {
        return {};
    }

    if (role == MenuRole) { // TODO this should be Qt::DisplayRole
        return actions.at(row)->text();
    } else if (role == ActionRole) {
        return QVariant::fromValue(actions.at(row));
    }

    return {};
}

void AppMenuModel::updateApplicationMenu(const QString &serviceName, const QString &menuObjectPath)
{
    if (m_serviceName == serviceName && m_menuObjectPath == menuObjectPath) {
        if (m_importer) {
            QMetaObject::invokeMethod(m_importer.get(), "updateMenu", Qt::QueuedConnection);
        }
        return;
    }

    if (serviceName.isEmpty() || menuObjectPath.isEmpty()) {
        if (shouldUseGenericMenu(m_tasksModel->activeTask())) {
            applyGenericMenu();
            return;
        }

        if (!m_menuAvailable) {
            return;
        }

        clearApplicationMenu();
    } else {
        m_usingGenericMenu = false;
        m_usingDesktopMenu = false;
        m_serviceName = serviceName;
        m_menuObjectPath = menuObjectPath;
        m_serviceWatcher->setWatchedServices(QStringList({m_serviceName}));

        m_importer = std::make_unique<KDBusMenuImporter>(serviceName, menuObjectPath);
        m_menu = m_importer->menu();
        QMetaObject::invokeMethod(m_importer.get(), "updateMenu", Qt::QueuedConnection);

        connect(m_importer.get(), &DBusMenuImporter::menuUpdated, this, [this](QMenu *menu) {
            QMenu *currentImporterMenu = m_importer ? m_importer->menu() : nullptr;
            if (!currentImporterMenu || menu != currentImporterMenu) {
                return;
            }

            if (currentImporterMenu->isEmpty()) {
                // naughty apps may pretend to have a menu but then don't actually give us one.
                if (shouldUseGenericMenu(m_tasksModel->activeTask())) {
                    ensureGenericMenuCreated();
                    m_usingGenericMenu = true;
                    m_menu = m_genericMenu.get();
                    setMenuAvailable(true);
                    setVisible(true);
                    Q_EMIT modelNeedsUpdate();
                    return;
                }
                if (m_menuAvailable) {
                    Q_EMIT modelNeedsUpdate();
                    return;
                }
                setMenuAvailable(false);
                return;
            } else {
                // if it somehow comes later, make it available again
                m_usingGenericMenu = false;
                m_menu = currentImporterMenu;
                setMenuAvailable(true);
            }

            // cache first layer of sub menus, which we'll be popping up
            const auto actions = m_menu->actions();
            for (QAction *a : actions) {
                // signal dataChanged when the action changes
                connect(a, &QAction::changed, this, [this, a] {
                    if (m_menuAvailable && m_menu) {
                        const int actionIdx = m_menu->actions().indexOf(a);
                        if (actionIdx > -1) {
                            const QModelIndex modelIdx = index(actionIdx, 0);
                            Q_EMIT dataChanged(modelIdx, modelIdx);
                        }
                    }
                });

                connect(a, &QAction::destroyed, this, &AppMenuModel::modelNeedsUpdate);

                if (a->menu()) {
                    m_importer->updateMenu(a->menu());
                }
            }

            setMenuAvailable(true);
            Q_EMIT modelNeedsUpdate();
        });

        connect(m_importer.get(), &DBusMenuImporter::actionActivationRequested, this, [this](QAction *action) {
            if (!m_menuAvailable || !m_menu || !action) {
                return;
            }

            const auto actions = m_menu->actions();
            auto it = std::ranges::find(actions, action);
            if (it != actions.end()) {
                Q_EMIT requestActivateIndex(it - actions.begin());
                return;
            }

            // If action is inside a submenu, find which top-level menu contains it
            for (int i = 0; i < actions.count(); ++i) {
                if (QMenu *sub = actions.at(i)->menu()) {
                    if (sub->findChildren<QAction *>().contains(action)) {
                        Q_EMIT requestActivateIndex(i);
                        return;
                    }
                }
            }
        });

        // Mark available while the layout loads; rowCount stays 0 until top-level actions exist.
        setMenuAvailable(true);
        setVisible(true);
        Q_EMIT modelNeedsUpdate();
    }
}

#include "moc_appmenumodel.cpp"
