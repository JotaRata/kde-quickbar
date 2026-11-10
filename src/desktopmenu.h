/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>

#include <memory>

class QMenu;
class KFilePlacesModel;

/*
 * Menu shown by the bar while the desktop (rather than an application
 * window) is active: a "Places" dropdown fed by KFilePlacesModel, matching
 * the Places panel of Dolphin and the file dialogs.
 */
class DesktopMenu : public QObject
{
    Q_OBJECT

public:
    explicit DesktopMenu(QObject *parent = nullptr);
    ~DesktopMenu() override;

    // Top-level menu handed to AppMenuModel. Owned by this object.
    QMenu *menu() const;

    // Re-apply a rebuild that had to be postponed because QuickBar had
    // the dropdown actions borrowed in an open popup. Safe to call at
    // any time: defers again while the popup is still up.
    void flushPendingRebuild();

private:
    void rebuildPlacesMenu();
    // Resolves the model row for a place URL captured at build time.
    QModelIndex placeIndexForUrl(const QUrl &url) const;
    void openPlace(const QUrl &url);

    std::unique_ptr<QMenu> m_menu;
    QPointer<QMenu> m_placesMenu;
    // Persistent submenus for the remote ("Network") and tag ("Tags")
    // place groups. Created once: rebuilds detach and re-attach them
    // (QMenu::clear() removes but does not delete addMenu()-created
    // submenus), so they never leak or duplicate.
    QPointer<QMenu> m_networkMenu;
    QPointer<QMenu> m_tagsMenu;
    QPointer<KFilePlacesModel> m_placesModel;
    // Number of actions added by the last rebuild. Used to tell an empty
    // Places model from a dropdown whose actions are borrowed by QuickBar.
    int m_lastBuiltCount = 0;
    // Place mounted on request from this menu; setupDone opens it after.
    QUrl m_pendingSetupUrl;
    bool m_rebuildPending = false;
};
