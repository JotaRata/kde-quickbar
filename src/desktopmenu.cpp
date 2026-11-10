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

    // Mirrors Plasma's desktop context menu (the "Standard Menu"
    // containment action): edit mode, wallpaper actions, desktop
    // configuration and display configuration.
    m_editMenu = m_menu->addMenu(i18n("&Edit"));
    m_editMenu->menuAction()->setVisible(false);
    connect(m_editMenu, &QMenu::aboutToShow, this, &DesktopMenu::rebuildEditMenu);

    m_placesMenu = m_menu->addMenu(i18n("&Places"));
    m_networkMenu = m_placesMenu->addMenu(i18n("&Network"));
    m_networkMenu->setIcon(QIcon::fromTheme(QStringLiteral("folder-network")));
    m_tagsMenu = m_placesMenu->addMenu(i18n("&Tags"));
    m_tagsMenu->setIcon(QIcon::fromTheme(QStringLiteral("tag")));

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
    if (m_editMenu) {
        m_editMenu->clear();
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
    rebuildEditMenu();
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
    // the Edit dropdown mirrors. An empty default plugin avoids creating
    // a containment as a side effect.
    const QString activity = KActivities::Consumer().currentActivity();
    return corona->containmentForScreen(anchor->screen(), activity, QString());
}

void DesktopMenu::rebuildEditMenu()
{
    if (!m_editMenu || m_editMenu->isVisible()) {
        return;
    }

    m_editMenu->clear();

    Plasma::Containment *desktop = desktopContainment();
    if (!desktop) {
        m_editMenu->menuAction()->setVisible(false);
        return;
    }

    // "Enter Edit Mode"/"Exit Edit Mode". DesktopView installs this action
    // on the desktop containment; it already flips its own text, icon and
    // enabled state. Fall back to the corona-wide action if the view has
    // not installed it yet.
    QAction *editModeAction = desktop->internalAction(QStringLiteral("desktop edit mode"));
    if (!editModeAction && desktop->corona()) {
        editModeAction = desktop->corona()->action(QStringLiteral("edit mode"));
    }
    if (editModeAction) {
        m_editMenu->addAction(editModeAction);
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
        }
    }

    // "Desktop and Wallpaper" (the containment's configure action, labelled
    // by the desktop containment itself) and "Display Configuration".
    QAction *configureAction = desktop->internalAction(QStringLiteral("configure"));
    const bool displayConfigurationAvailable =
        KAuthorized::authorizeControlModule(QStringLiteral("kcm_kscreen.desktop")) && KService::serviceByStorageId(QStringLiteral("kcm_kscreen"));
    if (configureAction || displayConfigurationAvailable) {
        if (!m_editMenu->isEmpty()) {
            m_editMenu->addSeparator();
        }
        if (configureAction) {
            m_editMenu->addAction(configureAction);
        }
        if (displayConfigurationAvailable) {
            m_editMenu->addAction(m_displayConfigurationAction);
        }
    }

    m_editMenu->menuAction()->setVisible(!m_editMenu->isEmpty());
}

void DesktopMenu::flushPendingRebuild()
{
    if (m_rebuildPending) {
        rebuildPlacesMenu();
    }
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
    if (m_placesMenu->actions().isEmpty() && m_lastBuiltCount > 0) {
        m_rebuildPending = true;
        return;
    }
    m_rebuildPending = false;

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
