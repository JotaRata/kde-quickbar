/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "desktopmenu.h"

#include <KAuthorized>
#include <KDialogJobUiDelegate>
#include <KFilePlacesModel>
#include <KIO/ApplicationLauncherJob>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KService>

#include <QAbstractItemModel>
#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QQmlListProperty>

#include <Plasma/Corona>

#include <plasmaactivities/consumer.h>

DesktopMenu::DesktopMenu(QObject *parent)
    : QObject(parent)
{
    m_menu = std::make_unique<QMenu>();
    m_menu->setObjectName(QStringLiteral("quickbar-desktop-menu"));

    m_activitiesConsumer = new KActivities::Consumer(this);
    // Whether "Show Activity Switcher" is offered at all depends on the
    // activity count, exactly like Plasma's own desktop context menu.
    connect(m_activitiesConsumer,
            &KActivities::Consumer::activitiesChanged,
            this,
            &DesktopMenu::refreshDesktopActions,
            Qt::UniqueConnection);

    // Desktop file operations (Create New, Move to Trash, Delete,
    // Properties, Empty Trash), borrowed from the folder view's actions
    // while the desktop uses one. Selection-dependent operations stay
    // greyed out until desktop icons are selected.
    m_fileMenu = m_menu->addMenu(i18n("&File"));
    m_fileMenu->menuAction()->setVisible(false);
    connect(m_fileMenu, &QMenu::aboutToShow, this, &DesktopMenu::rebuildFileMenu);

    // Desktop editing (Undo, Cut, Copy, Paste) and Plasma's desktop
    // context menu (edit mode, wallpaper actions, desktop configuration
    // and display configuration).
    m_editMenu = m_menu->addMenu(i18n("&Edit"));
    m_editMenu->menuAction()->setVisible(false);
    connect(m_editMenu, &QMenu::aboutToShow, this, &DesktopMenu::rebuildEditMenu);

    // Desktop view options: the folder view's Icons options flattened
    // into the dropdown, plus the activity manager.
    m_viewMenu = m_menu->addMenu(i18n("&View"));
    m_viewMenu->menuAction()->setVisible(false);
    connect(m_viewMenu, &QMenu::aboutToShow, this, &DesktopMenu::rebuildViewMenu);

    m_placesMenu = m_menu->addMenu(i18n("&Places"));
    m_networkMenu = m_placesMenu->addMenu(i18n("&Network"));
    m_networkMenu->setIcon(QIcon::fromTheme(QStringLiteral("folder-network")));
    m_tagsMenu = m_placesMenu->addMenu(i18n("&Tags"));
    m_tagsMenu->setIcon(QIcon::fromTheme(QStringLiteral("tag")));

    m_newAction = new QAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18n("&New"), this);
    m_newAction->setVisible(false);
    m_propertiesAction = new QAction(QIcon::fromTheme(QStringLiteral("document-properties")), i18n("&Properties"), this);
    connect(m_propertiesAction, &QAction::triggered, this, [this]() {
        if (m_folderModel) {
            // FolderModel::openPropertiesDialog(), for the current
            // desktop selection.
            QMetaObject::invokeMethod(m_folderModel, "openPropertiesDialog");
        }
    });

    m_displayConfigurationAction =
        new QAction(QIcon::fromTheme(QStringLiteral("preferences-desktop-display")), i18n("Display Configuration"), this);
    connect(m_displayConfigurationAction, &QAction::triggered, this, [] {
        if (auto service = KService::serviceByStorageId(QStringLiteral("kcm_kscreen"))) {
            auto *job = new KIO::ApplicationLauncherJob(service);
            job->start();
        }
    });

    m_placesModel = new KFilePlacesModel(this);

    // Places come and go (bookmarks edited, devices plugged/unplugged):
    // rebuild the dropdown when the model changes. rebuildPlacesMenu()
    // itself keeps this safe while QuickBar shows the dropdown.
    connect(m_placesModel, &QAbstractItemModel::rowsInserted, this, &DesktopMenu::rebuildPlacesMenu);
    connect(m_placesModel, &QAbstractItemModel::rowsRemoved, this, &DesktopMenu::rebuildPlacesMenu);
    connect(m_placesModel, &QAbstractItemModel::modelReset, this, &DesktopMenu::rebuildPlacesMenu);
    connect(m_placesModel, &QAbstractItemModel::layoutChanged, this, &DesktopMenu::rebuildPlacesMenu);
    // No role filter: place labels, icons and device accessibility all
    // affect the dropdown contents.
    connect(m_placesModel, &QAbstractItemModel::dataChanged, this, &DesktopMenu::rebuildPlacesMenu);

    // Clicking a device that needs mounting: mount first, then open.
    // Only setups requested from this menu trigger the open, so mounts
    // started elsewhere (e.g. Dolphin) do not spawn a window here.
    connect(m_placesModel, &KFilePlacesModel::setupDone, this, [this](const QModelIndex &index, bool success) {
        if (!success || !index.isValid()) {
            m_pendingSetupUrl.clear();
            return;
        }
        const QUrl url = m_placesModel->url(index);
        if (url == m_pendingSetupUrl) {
            m_pendingSetupUrl.clear();
            openPlace(url);
        }
    });

    rebuildPlacesMenu();
}

DesktopMenu::~DesktopMenu()
{
    // Drop the actions while they are still ours, before the menus die
    // with this object.
    if (m_fileMenu) {
        m_fileMenu->clear();
    }
    if (m_editMenu) {
        m_editMenu->clear();
    }
    if (m_viewMenu) {
        m_viewMenu->clear();
    }
    if (m_placesMenu) {
        m_placesMenu->clear();
    }
    if (m_menu) {
        m_menu->clear();
    }
}

QMenu *DesktopMenu::menu() const
{
    return m_menu.get();
}

void DesktopMenu::setAnchorContainment(Plasma::Containment *containment)
{
    if (m_anchorContainment == containment) {
        return;
    }
    if (m_anchorContainment) {
        disconnect(m_anchorContainment, nullptr, this, nullptr);
    }
    m_anchorContainment = containment;
    if (containment) {
        connect(containment, &Plasma::Containment::screenChanged, this, &DesktopMenu::refreshDesktopActions, Qt::UniqueConnection);
        if (Plasma::Corona *corona = containment->corona()) {
            // A desktop containment may appear (activity added, screen
            // hotplugged) after the anchor was set.
            connect(corona, &Plasma::Corona::containmentAdded, this, &DesktopMenu::refreshDesktopActions, Qt::UniqueConnection);
            connect(corona, &Plasma::Corona::containmentCreated, this, &DesktopMenu::refreshDesktopActions, Qt::UniqueConnection);
        }
    }
    refreshDesktopActions();
}

void DesktopMenu::refreshDesktopActions()
{
    rebuildFileMenu();
    rebuildEditMenu();
    rebuildViewMenu();
}

Plasma::Containment *DesktopMenu::desktopContainment() const
{
    Plasma::Containment *anchor = m_anchorContainment;
    if (!anchor) {
        return nullptr;
    }
    // The widget can also live directly on the desktop.
    if (anchor->containmentType() == Plasma::Containment::Desktop) {
        return anchor;
    }
    Plasma::Corona *corona = anchor->corona();
    if (!corona) {
        return nullptr;
    }
    // Desktop containments are per screen and per activity; the panel's
    // screen and the current activity select the one whose context menu
    // the dropdowns mirror. An empty default plugin avoids creating
    // a containment as a side effect.
    const QString activity = m_activitiesConsumer->currentActivity();
    return corona->containmentForScreen(anchor->screen(), activity, QString());
}

Plasma::Containment *DesktopMenu::folderViewContainment() const
{
    // Only the folder view ("Icons" desktop) has the file-management
    // contextual actions; the plain desktop containment has none. This
    // is also Plasma's default desktop containment.
    Plasma::Containment *desktop = desktopContainment();
    if (desktop && desktop->pluginName() == QLatin1String("org.kde.plasma.folder")) {
        return desktop;
    }
    return nullptr;
}

QObject *DesktopMenu::folderModel(Plasma::Containment *folder)
{
    const QList<QAction *> actions = folder->contextualActions();
    for (QAction *action : actions) {
        // KNewFileMenu is created with the FolderModel as its QObject
        // parent, so this action leads straight to the model instance.
        if (action && action->objectName() == QLatin1String("newMenu")) {
            return action->parent();
        }
    }
    return nullptr;
}

void DesktopMenu::watchFolderSelection(QObject *model)
{
    if (m_folderModel == model) {
        return;
    }
    if (m_folderModel) {
        disconnect(m_folderModel, SIGNAL(selectionChanged()), this, SLOT(onFolderSelectionChanged()));
        m_folderModel.clear();
        m_trashAction.clear();
        m_delAction.clear();
        m_renameAction.clear();
        m_cutAction.clear();
        m_copyAction.clear();
    }
    if (model) {
        m_folderModel = model;
        // Selections are made on the desktop long after the last rebuild,
        // so keep the selection-dependent entries' enabled state live.
        connect(m_folderModel, SIGNAL(selectionChanged()), this, SLOT(onFolderSelectionChanged()));
    }
}

void DesktopMenu::onFolderSelectionChanged()
{
    if (!m_folderModel || m_emittingContextMenu) {
        return;
    }
    const bool hasSelection = folderHasSelection(m_folderModel);
    const QPointer<QAction> gated[] = {m_trashAction, m_delAction, m_renameAction, m_cutAction, m_copyAction};
    for (const QPointer<QAction> &action : gated) {
        if (action) {
            action->setEnabled(hasSelection);
        }
    }
    m_propertiesAction->setEnabled(hasSelection);
}

QAction *DesktopMenu::modelAction(QObject *model, const QString &name)
{
    QAction *action = nullptr;
    QMetaObject::invokeMethod(model, "action", Q_RETURN_ARG(QAction *, action), Q_ARG(QString, name));
    return action;
}

bool DesktopMenu::folderHasSelection(QObject *model) const
{
    bool hasSelection = false;
    QMetaObject::invokeMethod(model, "hasSelection", Q_RETURN_ARG(bool, hasSelection));
    return hasSelection;
}

bool DesktopMenu::canRebuildDropdown(QMenu *menu, int lastBuiltCount)
{
    // While QuickBar shows the dropdown, its actions are borrowed (removed
    // from this menu into the popup). Deleting or re-adding them now would
    // leave duplicates once the popup returns them on close, so postpone
    // the rebuild to the next flushPendingRebuild(). An empty dropdown is
    // fine to rebuild: the last rebuild then also produced no actions.
    if (menu->actions().isEmpty() && lastBuiltCount > 0) {
        m_rebuildPending = true;
        return false;
    }
    return true;
}

void DesktopMenu::rebuildFileMenu()
{
    if (!m_fileMenu || m_fileMenu->isVisible() || !canRebuildDropdown(m_fileMenu, m_lastFileBuiltCount)) {
        return;
    }

    m_fileMenu->clear();

    Plasma::Containment *folder = folderViewContainment();
    QObject *model = folder ? folderModel(folder) : nullptr;
    if (!model) {
        watchFolderSelection(nullptr);
        m_lastFileBuiltCount = 0;
        m_fileMenu->menuAction()->setVisible(false);
        return;
    }
    watchFolderSelection(model);

    // Plasma refreshes these actions' text/enabled/visible state only
    // right before it shows them in its own menu (FolderViewLayer::
    // updateContextualActions → FolderModel::updateActions); do the
    // same before reading them. The aboutToShow handlers also CLEAR the
    // desktop selection, so the selection state is read first.
    const bool hasSelection = folderHasSelection(model);
    m_emittingContextMenu = true;
    Q_EMIT folder->contextualActionsAboutToShow();
    m_emittingContextMenu = false;

    // The selection-dependent operations stay greyed out until desktop
    // icons are selected; with a selection, Plasma's own enabled state
    // applies.
    int built = 0;

    // "Create New": QuickBar's own action so it can carry this domain's
    // label, opening the real KNewFileMenu submenu of the folder view
    // (shared with Plasma's own desktop context menu).
    if (QAction *newMenu = modelAction(model, QStringLiteral("newMenu"))) {
        m_newAction->setMenu(newMenu->menu());
        // Same icon and enabled state as Plasma's own "Create New".
        m_newAction->setIcon(newMenu->icon());
        m_newAction->setEnabled(newMenu->isEnabled());
        m_newAction->setVisible(newMenu->isVisible());
        m_fileMenu->addAction(m_newAction);
        ++built;
        m_fileMenu->addSeparator();
    }

    // Move to Trash, Delete and Properties, for the desktop selection.
    m_trashAction = modelAction(model, QStringLiteral("trash"));
    if (m_trashAction) {
        m_trashAction->setEnabled(hasSelection);
        m_fileMenu->addAction(m_trashAction);
        ++built;
    }
    m_delAction = modelAction(model, QStringLiteral("del"));
    if (m_delAction) {
        m_delAction->setEnabled(hasSelection);
        m_fileMenu->addAction(m_delAction);
        ++built;
    }
    m_renameAction = modelAction(model, QStringLiteral("rename"));
    if (m_renameAction) {
        m_renameAction->setEnabled(hasSelection);
        m_fileMenu->addAction(m_renameAction);
        ++built;
    }
    m_propertiesAction->setEnabled(hasSelection);
    m_fileMenu->addAction(m_propertiesAction);
    ++built;

    // Empty Trash. Plasma only offers it while the desktop shows the
    // trash link; keep exactly that behavior.
    if (QAction *emptyTrash = modelAction(model, QStringLiteral("emptyTrash"))) {
        if (emptyTrash->isVisible()) {
            m_fileMenu->addSeparator();
            m_fileMenu->addAction(emptyTrash);
            ++built;
        }
    }

    m_lastFileBuiltCount = built;
    m_fileMenu->menuAction()->setVisible(built > 0);
}

void DesktopMenu::rebuildEditMenu()
{
    if (!m_editMenu || m_editMenu->isVisible() || !canRebuildDropdown(m_editMenu, m_lastEditBuiltCount)) {
        return;
    }

    m_editMenu->clear();

    int built = 0;

    Plasma::Containment *folder = folderViewContainment();
    QObject *model = folder ? folderModel(folder) : nullptr;
    if (model) {
        // Plasma refreshes these actions' text/enabled/visible state only
        // right before it shows them in its own menu (FolderViewLayer::
        // updateContextualActions → FolderModel::updateActions); do the
        // same before reading them. The aboutToShow handlers also CLEAR
        // the desktop selection, so the selection state is read first.
        const bool hasSelection = folderHasSelection(model);
        m_emittingContextMenu = true;
        Q_EMIT folder->contextualActionsAboutToShow();
        m_emittingContextMenu = false;

        // Undo and the clipboard operations for the desktop selection.
        // Cut and Copy stay greyed out until desktop icons are selected;
        // Paste is selection-independent and keeps Plasma's own state.
        const QList<QString> names = {QStringLiteral("undo"), QStringLiteral("cut"), QStringLiteral("copy"), QStringLiteral("paste")};
        for (const QString &name : names) {
            QAction *action = modelAction(model, name);
            if (!action) {
                continue;
            }
            if (name == QLatin1String("cut") || name == QLatin1String("copy")) {
                action->setEnabled(hasSelection);
                if (name == QLatin1String("cut")) {
                    m_cutAction = action;
                } else {
                    m_copyAction = action;
                }
            }
            m_editMenu->addAction(action);
            ++built;
        }
    }

    Plasma::Containment *desktop = desktopContainment();
    if (!desktop) {
        m_lastEditBuiltCount = built;
        m_editMenu->menuAction()->setVisible(built > 0);
        return;
    }

    if (built > 0) {
        m_editMenu->addSeparator();
    }

    // Mirrors Plasma's desktop context menu (the "Standard Menu"
    // containment action): edit mode, wallpaper actions, desktop
    // configuration and display configuration.
    //
    // "Enter Edit Mode"/"Exit Edit Mode". DesktopView installs this
    // action on the desktop containment; it already flips its own text,
    // icon and enabled state. Fall back to the corona-wide action if the
    // view has not installed it yet.
    QAction *editModeAction = desktop->internalAction(QStringLiteral("desktop edit mode"));
    if (!editModeAction && desktop->corona()) {
        editModeAction = desktop->corona()->action(QStringLiteral("edit mode"));
    }
    if (editModeAction) {
        m_editMenu->addAction(editModeAction);
        ++built;
    }

    // Wallpaper-provided actions: "Open Wallpaper Image" and, for the
    // slideshow plugin, "Next Wallpaper Image". Same source as Plasma's
    // own desktop context menu.
    bool haveWallpaperActions = false;
    QObject *wallpaper = desktop->wallpaperPlugin().isEmpty() ? nullptr : desktop->property("wallpaperGraphicsObject").value<QObject *>();
    if (wallpaper) {
        QQmlListProperty<QAction> wallpaperActions = wallpaper->property("contextualActions").value<QQmlListProperty<QAction>>();
        const int count = wallpaperActions.count(&wallpaperActions);
        for (int i = 0; i < count; ++i) {
            QAction *action = wallpaperActions.at(&wallpaperActions, i);
            if (!action) {
                continue;
            }
            if (!haveWallpaperActions && !m_editMenu->isEmpty()) {
                m_editMenu->addSeparator();
            }
            m_editMenu->addAction(action);
            haveWallpaperActions = true;
            ++built;
        }
    }

    // "Desktop and Wallpaper" (the containment's configure action,
    // labelled by the desktop containment itself) and "Display
    // Configuration".
    QAction *configureAction = desktop->internalAction(QStringLiteral("configure"));
    const bool displayConfigurationAvailable =
        KAuthorized::authorizeControlModule(QStringLiteral("kcm_kscreen.desktop")) && KService::serviceByStorageId(QStringLiteral("kcm_kscreen"));
    if (configureAction || displayConfigurationAvailable) {
        if (!m_editMenu->isEmpty()) {
            m_editMenu->addSeparator();
        }
        if (configureAction) {
            m_editMenu->addAction(configureAction);
            ++built;
        }
        if (displayConfigurationAvailable) {
            m_editMenu->addAction(m_displayConfigurationAction);
            ++built;
        }
    }

    m_lastEditBuiltCount = built;
    m_editMenu->menuAction()->setVisible(built > 0);
}

void DesktopMenu::rebuildViewMenu()
{
    if (!m_viewMenu || m_viewMenu->isVisible() || !canRebuildDropdown(m_viewMenu, m_lastViewBuiltCount)) {
        return;
    }

    m_viewMenu->clear();

    int built = 0;

    if (Plasma::Containment *folder = folderViewContainment()) {
        Q_EMIT folder->contextualActionsAboutToShow();

        // The folder view's "Icons" action (the only unnamed contextual
        // action with a submenu) carries the view-options menu: Sort By,
        // Icon Size, Arrange, Sort Alignment, Show Previews and Locked.
        // Flatten its entries into this dropdown as live proxies instead
        // of borrowing them: stealing them would empty Plasma's own
        // "Icons" submenu.
        const QList<QAction *> actions = folder->contextualActions();
        for (QAction *action : actions) {
            if (action && action->objectName().isEmpty() && action->menu()) {
                const QList<QAction *> entries = action->menu()->actions();
                for (QAction *entry : entries) {
                    if (QAction *proxy = proxyViewAction(entry, m_viewMenu)) {
                        m_viewMenu->addAction(proxy);
                        ++built;
                    }
                }
            }
        }
    }

    // "Show Activity Switcher", hidden with a single activity exactly
    // like Plasma's own desktop context menu.
    if (m_activitiesConsumer->activities().length() > 1) {
        Plasma::Corona *corona = nullptr;
        if (Plasma::Containment *desktop = desktopContainment()) {
            corona = desktop->corona();
        } else if (m_anchorContainment) {
            corona = m_anchorContainment->corona();
        }
        if (QAction *activitiesAction = corona ? corona->action(QStringLiteral("manage activities")) : nullptr) {
            if (built > 0) {
                m_viewMenu->addSeparator();
            }
            m_viewMenu->addAction(activitiesAction);
            ++built;
        }
    }

    m_lastViewBuiltCount = built;
    m_viewMenu->menuAction()->setVisible(built > 0);
}

QAction *DesktopMenu::proxyViewAction(QAction *source, QMenu *owner)
{
    if (!source) {
        return nullptr;
    }

    if (source->isSeparator()) {
        QAction *separator = new QAction(owner);
        separator->setSeparator(true);
        return separator;
    }

    QAction *proxy = new QAction(owner);
    syncProxyAction(source, proxy);
    // Mirror every later state change of the shared source (checked
    // toggles in the exclusive groups, icon size picked elsewhere, …).
    connect(source, &QAction::changed, proxy, [this, source, proxy]() {
        syncProxyAction(source, proxy);
    });
    // Triggering the proxy drives the real action, which applies the
    // change and updates every proxy through the signal above.
    connect(proxy, &QAction::triggered, source, [source]() {
        source->trigger();
    });

    if (QMenu *sourceMenu = source->menu()) {
        // A QMenu cannot be parented to a QAction (it is a QWidget), so
        // tie its lifetime to the proxy action instead.
        QMenu *proxyMenu = new QMenu();
        connect(proxy, &QObject::destroyed, proxyMenu, &QObject::deleteLater);
        const QList<QAction *> entries = sourceMenu->actions();
        for (QAction *entry : entries) {
            if (QAction *entryProxy = proxyViewAction(entry, proxyMenu)) {
                proxyMenu->addAction(entryProxy);
            }
        }
        proxy->setMenu(proxyMenu);
    }

    return proxy;
}

void DesktopMenu::syncProxyAction(const QAction *source, QAction *proxy)
{
    proxy->setText(source->text());
    proxy->setIcon(source->icon());
    proxy->setEnabled(source->isEnabled());
    proxy->setVisible(source->isVisible());
    proxy->setCheckable(source->isCheckable());
    proxy->setChecked(source->isChecked());
}

void DesktopMenu::flushPendingRebuild()
{
    if (!m_rebuildPending) {
        return;
    }
    m_rebuildPending = false;
    rebuildPlacesMenu();
    rebuildFileMenu();
    rebuildEditMenu();
    rebuildViewMenu();
}

void DesktopMenu::rebuildPlacesMenu()
{
    if (!m_placesModel || !m_placesMenu || !m_networkMenu || !m_tagsMenu) {
        return;
    }

    // While QuickBar shows the dropdown, its actions are borrowed (removed
    // from this menu into the popup). Deleting or re-adding them now would
    // leave duplicates once the popup returns them on close, so postpone
    // the rebuild to the next flushPendingRebuild(). An empty model is fine
    // to rebuild: the last rebuild then also produced no actions.
    if (!canRebuildDropdown(m_placesMenu, m_lastBuiltCount)) {
        return;
    }

    // The Network/Tags submenus are children of m_placesMenu: clear()
    // detaches them from the dropdown without deleting them, so they
    // survive rebuilds. Their own entries are never borrowed by QuickBar
    // (only the dropdown's level-1 actions are), which is why rebuilding
    // them here is safe.
    m_placesMenu->clear();
    m_networkMenu->clear();
    m_tagsMenu->clear();

    // One entry builder for the flat dropdown and the group submenus.
    auto addPlaceAction = [this](QMenu *target, const QModelIndex &index) {
        const QUrl url = m_placesModel->url(index);
        auto *action = target->addAction(m_placesModel->text(index));
        action->setIcon(m_placesModel->icon(index));
        // Devices needing a mount stay clickable: triggering mounts them
        // (requestSetup) and setupDone opens them. Only mid-setup and
        // mid-teardown entries are not actionable.
        const int accessibility = m_placesModel->data(index, KFilePlacesModel::DeviceAccessibilityRole).toInt();
        action->setEnabled(accessibility != KFilePlacesModel::SetupInProgress
                            && accessibility != KFilePlacesModel::TeardownInProgress);

        connect(action, &QAction::triggered, this, [this, url]() {
            const QModelIndex placeIndex = placeIndexForUrl(url);
            if (!placeIndex.isValid()) {
                // The place vanished since the menu was built.
                return;
            }
            if (m_placesModel->setupNeeded(placeIndex)) {
                m_pendingSetupUrl = url;
                m_placesModel->requestSetup(placeIndex);
                return;
            }
            openPlace(url);
        });
    };

    KFilePlacesModel::GroupType previousGroup = KFilePlacesModel::PlacesType;
    bool havePrevious = false;
    bool networkAttached = false;
    bool tagsAttached = false;
    int builtCount = 0;

    // Groups rendered as persistent submenus instead of flat entries:
    // attached once at the position of the group's first visible entry,
    // skipped entirely while the group has no visible entries.
    auto attachSubmenuGroup = [&](QMenu *submenu, bool &attached, const QModelIndex &idx, KFilePlacesModel::GroupType groupType) {
        if (!attached) {
            attached = true;
            if (havePrevious) {
                m_placesMenu->addSeparator();
            }
            m_placesMenu->addMenu(submenu);
            previousGroup = groupType;
            havePrevious = true;
        }
        addPlaceAction(submenu, idx);
        ++builtCount;
    };

    const int rows = m_placesModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = m_placesModel->index(row, 0);
        if (!index.isValid() || m_placesModel->isHidden(index)) {
            continue;
        }

        const KFilePlacesModel::GroupType group = m_placesModel->groupType(index);

        if (group == KFilePlacesModel::RemoteType) {
            // Remote places (sftp, remote:/, …) live in the Network submenu.
            attachSubmenuGroup(m_networkMenu, networkAttached, index, group);
            continue;
        }
        if (group == KFilePlacesModel::TagsType) {
            // Tag places live in the Tags submenu.
            attachSubmenuGroup(m_tagsMenu, tagsAttached, index, group);
            continue;
        }

        // Section separators between the Places panel groups, e.g.
        // Places vs Devices, like the Dolphin sidebar headers.
        if (havePrevious && group != previousGroup) {
            m_placesMenu->addSeparator();
        }
        previousGroup = group;
        havePrevious = true;

        addPlaceAction(m_placesMenu, index);
        ++builtCount;
    }

    m_lastBuiltCount = builtCount;
}

QModelIndex DesktopMenu::placeIndexForUrl(const QUrl &url) const
{
    if (!m_placesModel || url.isEmpty()) {
        return {};
    }
    const int rows = m_placesModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = m_placesModel->index(row, 0);
        if (index.isValid() && m_placesModel->url(index) == url) {
            return index;
        }
    }
    return {};
}

void DesktopMenu::openPlace(const QUrl &url)
{
    if (url.isEmpty()) {
        return;
    }
    auto *job = new KIO::OpenUrlJob(url);
    job->setUiDelegate(new KDialogJobUiDelegate);
    job->start();
}

#include "moc_desktopmenu.cpp"
