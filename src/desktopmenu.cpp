/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "desktopmenu.h"

#include <KDialogJobUiDelegate>
#include <KFilePlacesModel>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>

#include <QAbstractItemModel>
#include <QAction>
#include <QIcon>
#include <QMenu>

DesktopMenu::DesktopMenu(QObject *parent)
    : QObject(parent)
{
    m_menu = std::make_unique<QMenu>();
    m_menu->setObjectName(QStringLiteral("quickbar-desktop-menu"));

    m_placesMenu = m_menu->addMenu(i18n("&Places"));
    m_networkMenu = m_placesMenu->addMenu(i18n("&Network"));
    m_networkMenu->setIcon(QIcon::fromTheme(QStringLiteral("folder-network")));

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

void DesktopMenu::flushPendingRebuild()
{
    if (m_rebuildPending) {
        rebuildPlacesMenu();
    }
}

void DesktopMenu::rebuildPlacesMenu()
{
    if (!m_placesModel || !m_placesMenu || !m_networkMenu) {
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

    // m_networkMenu is a child of m_placesMenu: clear() detaches it from
    // the dropdown without deleting it, so it survives rebuilds. Its own
    // entries are never borrowed by QuickBar (only the dropdown's level-1
    // actions are), which is why rebuilding it here is safe.
    m_placesMenu->clear();
    m_networkMenu->clear();

    // One entry builder for both the flat dropdown and the Network submenu.
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
    int builtCount = 0;

    const int rows = m_placesModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = m_placesModel->index(row, 0);
        if (!index.isValid() || m_placesModel->isHidden(index)) {
            continue;
        }

        const KFilePlacesModel::GroupType group = m_placesModel->groupType(index);

        if (group == KFilePlacesModel::RemoteType) {
            // Remote places live inside the Network submenu, attached once
            // at the position of the first visible remote entry. Skipped
            // entirely while all remote entries are hidden.
            if (!networkAttached) {
                networkAttached = true;
                if (havePrevious) {
                    m_placesMenu->addSeparator();
                }
                m_placesMenu->addMenu(m_networkMenu);
                previousGroup = group;
                havePrevious = true;
            }
            addPlaceAction(m_networkMenu, index);
            ++builtCount;
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
