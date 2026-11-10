/*
    SPDX-FileCopyrightText: 2016 Chinmoy Ranjan Pradhan <chinmoyrp65@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <KWindowSystem>
#include <QAbstractListModel>
#include <QAction>
#include <QPointer>
#include <QRect>
#include <QStringList>
#include <qqmlregistration.h>
#include <tasksmodel.h>
#include <virtualdesktopinfo.h>

#include <Plasma/Containment>

class QMenu;
class QModelIndex;
class QDBusServiceWatcher;
class KDBusMenuImporter;
class MenuShortcutBridge;
class DesktopMenu;

class AppMenuModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool menuAvailable READ menuAvailable WRITE setMenuAvailable NOTIFY menuAvailableChanged)
    Q_PROPERTY(bool visible READ visible NOTIFY visibleChanged)
    Q_PROPERTY(bool activeWindowMaximized READ activeWindowMaximized NOTIFY activeWindowMaximizedChanged)
    Q_PROPERTY(bool menuForDisplay READ menuForDisplay NOTIFY menuForDisplayChanged)
    Q_PROPERTY(bool stickyMenuBar READ stickyMenuBar WRITE setStickyMenuBar NOTIFY stickyMenuBarChanged)
    Q_PROPERTY(bool showDesktopMenu READ showDesktopMenu WRITE setShowDesktopMenu NOTIFY showDesktopMenuChanged)
    Q_PROPERTY(bool enableGenericMenu READ enableGenericMenu WRITE setEnableGenericMenu NOTIFY enableGenericMenuChanged)
    Q_PROPERTY(bool enableMenuSearch READ enableMenuSearch WRITE setEnableMenuSearch NOTIFY enableMenuSearchChanged)
    Q_PROPERTY(QString applicationName READ applicationName NOTIFY applicationNameChanged)
    Q_PROPERTY(QVariant applicationIcon READ applicationIcon NOTIFY applicationIconChanged)
    Q_PROPERTY(bool hasActiveWindow READ hasActiveWindow NOTIFY hasActiveWindowChanged)
    Q_PROPERTY(QString excludedItemsRegex READ excludedItemsRegex WRITE setExcludedItemsRegex NOTIFY excludedItemsRegexChanged)
    Q_PROPERTY(bool allScreens READ allScreens WRITE setallScreens NOTIFY allScreensChanged)

    Q_PROPERTY(Plasma::Types::ItemStatus containmentStatus MEMBER m_containmentStatus NOTIFY containmentStatusChanged)
    Q_PROPERTY(QRect screenGeometry READ screenGeometry WRITE setScreenGeometry NOTIFY screenGeometryChanged)

public:
    // Dynamic property marking the model-owned search dropdown. The applet
    // must never proxy this menu: results are inserted into it while it is
    // visible, and a proxy would leave them in a hidden source menu.
    static constexpr char SearchMenuProperty[] = "quickbarSearchMenu";

    explicit AppMenuModel(QObject *parent = nullptr);
    ~AppMenuModel() override;

    enum AppMenuRole {
        MenuRole = Qt::UserRole + 1, // TODO this should be Qt::DisplayRole
        ActionRole,
        SearchRole,
    };

    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;

    void updateApplicationMenu(const QString &serviceName, const QString &menuObjectPath);

    bool menuAvailable() const;
    void setMenuAvailable(bool set);

    bool allScreens() const;
    void setallScreens(bool allScreens);

    bool visible() const;
    bool activeWindowMaximized() const;
    bool menuForDisplay() const;

    bool stickyMenuBar() const;
    void setStickyMenuBar(bool sticky);

    bool showDesktopMenu() const;
    void setShowDesktopMenu(bool show);

    // Containment the widget lives in (usually the panel). Forwarded to
    // the desktop menu so its Edit dropdown can mirror the desktop
    // containment on the same screen and current activity.
    void setContainment(Plasma::Containment *containment);

    bool enableGenericMenu() const;
    void setEnableGenericMenu(bool enable);

    bool enableMenuSearch() const;
    void setEnableMenuSearch(bool enable);

    QString applicationName() const;
    QVariant applicationIcon() const;

    // True while a task window is active, i.e. the application-name dropdown
    // has a window to act on.
    bool hasActiveWindow() const;

    // Lazily created dropdown opened from the application name label,
    // mirroring KWin's window operations menu (Alt+F3) through the task
    // manager. Owned by the model; the applet borrows it like the
    // CompactView menu.
    QMenu *windowMenu();

    // macOS-style system menu shown by the same application-name dropdown
    // while the desktop instead of a window is active. Owned by the model,
    // like the window menu.
    QMenu *systemMenu();

    QString excludedItemsRegex() const;
    void setExcludedItemsRegex(const QString &regex);

    QRect screenGeometry() const;
    void setScreenGeometry(QRect geometry);
    QList<QAction *> flatActionList();
    QList<QAction *> visibleActions() const;

Q_SIGNALS:
    void requestActivateIndex(int index);
    void bringToFocus(int index);
    void requestOpenAbout();
    void menuUpdated(QMenu *menu);

private Q_SLOTS:
    void onActiveWindowChanged();
    void setVisible(bool visible);
    void update();

Q_SIGNALS:
    void allScreensChanged();
    void menuAvailableChanged();
    void activeWindowMaximizedChanged();
    void menuForDisplayChanged();
    void stickyMenuBarChanged();
    void showDesktopMenuChanged();
    void enableGenericMenuChanged();
    void enableMenuSearchChanged();
    void applicationNameChanged();
    void applicationIconChanged();
    void hasActiveWindowChanged();
    void excludedItemsRegexChanged();
    void modelNeedsUpdate();
    void containmentStatusChanged();
    void screenGeometryChanged();
    void visibleChanged();

private:
    bool m_menuAvailable;
    bool m_activeWindowMaximized = false;
    bool m_maximizedRefreshQueued = false;
    bool m_allScreens = true;
    bool m_updatePending = false;
    bool m_visible = true;
    bool m_stickyMenuBar = false;
    bool m_showDesktopMenu = true;
    bool m_enableGenericMenu = true;
    bool m_enableMenuSearch = true;
    bool m_usingGenericMenu = false;
    bool m_usingDesktopMenu = false;
    QString m_applicationName;
    QVariant m_applicationIcon;
    QString m_excludedItemsRegex;
    QRegularExpression m_compiledExcludedRegex;

    Plasma::Types::ItemStatus m_containmentStatus = Plasma::Types::PassiveStatus;

    void setApplicationName(const QString &name);
    void setApplicationIcon(const QVariant &icon);
    void setHasActiveWindow(bool active);
    void setActiveWindowMaximized(bool maximized);
    // True when the given task is maximized, resolving group parents to
    // their active child. Invalid index returns false.
    bool isTaskMaximized(const QModelIndex &index) const;
    // Re-read the maximized state of the current active task and publish it.
    // Cheap and emission-gated: safe to call on any tasks-model change.
    void refreshActiveWindowMaximized();
    void ensureGenericMenuCreated();
    void applyGenericMenu();
    void ensureDesktopMenuCreated();
    void applyDesktopMenu();
    void clearApplicationMenu();
    bool shouldUseGenericMenu(const QModelIndex &activeTaskIndex) const;
    void quitActiveTask();
    void minimizeActiveTask();
    void maximizeActiveTask();
    void keepAboveActiveTask();
    void keepBelowActiveTask();
    void fullScreenActiveTask();
    void noBorderActiveTask();
    void excludeFromCaptureActiveTask();
    void moveActiveTask();
    void resizeActiveTask();
    // Open kcm_kwinrules for the active task's window (Wayland uuid); with
    // wholeApplication, preselect the whole-application rules tab.
    void openWindowRules(bool wholeApplication);
    // System-menu actions (desktop-focused name dropdown).
    void openAboutSystem();
    void openSystemSettings();
    void lockScreen();
    void logoutSession();
    void sleepSession();
    // Hibernate through powerdevil, falling back to logind (see
    // hibernateAvailable()).
    void hibernateSession();
    // Availability checks for the Sleep/Hibernate system menu entries:
    // powerdevil's freedesktop PowerManagement capability methods, falling
    // back to logind's Can* checks ("no" means not allowed).
    bool powerDevilCanDo(const char *method);
    bool logindAllows(const char *method);
    bool sleepAvailable();
    bool hibernateAvailable();
    void restartSession();
    void shutdownSession();
    // Connect actions carrying the quickbar action properties to the
    // task-manager slots. Works for any menu built with those properties,
    // e.g. the generic and window menus.
    void wireMenuActions(QMenu *menu);
    // Refresh enabled/checked state of the property-carrying actions in
    // `menu` from the active task capabilities.
    void updateMenuActionState(QMenu *menu);
    void ensureWindowMenuCreated();
    // Create (once) and wire the system menu; it is static — no per-task
    // state to refresh.
    void ensureSystemMenuCreated();
    // Show/hide the power entries of the system menu by re-checking
    // availability (powerdevil/logind capability queries) on each open.
    void updateSystemMenuState();
    // Refresh the window menu (if created) for the current active task.
    void updateWindowMenuState();
    // Rebuild the Desktops submenu (VirtualDesktopInfo desktop ids/names,
    // the task's desktops, "All Desktops", "Move to New Desktop") for the
    // current active task. Called on task changes and on submenu show.
    void rebuildDesktopsMenu();
    // Rebuild the Move-to-Screen submenu from the screen list. Requires
    // TaskManager::TasksModel::requestSendToOutput (Plasma 6.9+), discovered
    // at runtime: the entry stays hidden on older stacks.
    void rebuildScreensMenu();
    bool genericMenuHasDuplicateActions(const QMenu *menu) const;
    TaskManager::TasksModel *m_tasksModel;

    std::unique_ptr<QMenu> m_searchMenu;
    std::unique_ptr<QMenu> m_genericMenu;
    std::unique_ptr<QMenu> m_windowMenu;
    std::unique_ptr<QMenu> m_systemMenu;
    // Sleep/Hibernate entries of the system menu, for the availability check.
    QPointer<QAction> m_sleepAction;
    QPointer<QAction> m_hibernateAction;
    // Dynamic submenus of the window menu (owned by m_windowMenu).
    QPointer<QMenu> m_desktopsMenu;
    QPointer<QMenu> m_screensMenu;
    TaskManager::VirtualDesktopInfo *m_virtualDesktopInfo = nullptr;
    std::unique_ptr<DesktopMenu> m_desktopMenu;
    QPointer<Plasma::Containment> m_containment;
    MenuShortcutBridge *m_shortcutBridge = nullptr;
    QPointer<QMenu> m_menu;
    QPointer<QAction> m_searchAction;
    QList<QPointer<QAction>> m_currentSearchActions;

    bool m_hasActiveWindow = false;

    void removeSearchActionsFromMenu();
    void insertSearchActionsIntoMenu(const QString &filter = QString());
    bool includeSearchInModel() const;

    QDBusServiceWatcher *m_serviceWatcher;
    QString m_serviceName;
    QString m_menuObjectPath;

    std::unique_ptr<KDBusMenuImporter> m_importer;
};
