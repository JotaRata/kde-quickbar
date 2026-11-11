/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <Plasma/Containment>

#include <QObject>
#include <QPointer>
#include <QUrl>

#include <memory>

class QAction;
class QMenu;
class KFilePlacesModel;

namespace KActivities
{
class Consumer;
}

/*
 * Menu shown by the bar while the desktop (rather than an application
 * window) is active, macOS-Finder-style: a "File" dropdown with Create
 * New and the desktop-selection file operations (Move to Trash, Delete,
 * Properties, Empty Trash), an "Edit" dropdown with Undo/Cut/Copy/Paste
 * and Plasma's desktop context menu (edit mode, wallpaper actions,
 * desktop configuration, display configuration), a "View" dropdown with
 * the folder view's Icons options flattened into it plus the activity
 * manager, and a "Places" dropdown fed by KFilePlacesModel, matching
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

    // Containment the applet lives in (usually the panel). Used to locate
    // the desktop containment on the same screen and current activity,
    // whose actions the dropdowns mirror.
    void setAnchorContainment(Plasma::Containment *containment);

    // Rebuild the containment-backed dropdowns (File, Edit and View)
    // from the current desktop containment. Also hides them while no
    // matching desktop containment can be resolved.
    void refreshDesktopActions();

    // Re-apply a rebuild that had to be postponed because QuickBar had
    // the dropdown actions borrowed in an open popup. Safe to call at
    // any time: defers again while the popup is still up.
    void flushPendingRebuild();

private slots:
    void onFolderSelectionChanged();

private:
    // Live-gate for the selection-dependent file operations: selections
    // happen on the desktop long after the last dropdown rebuild, so the
    // FolderModel's selectionChanged() signal re-applies the enabled
    // state. Skips the clear triggered by our own
    // contextualActionsAboutToShow emission, which the gates already
    // handled from the pre-clear state.
    // Subscribe m_folderModel's selectionChanged() to the live gate.
    void watchFolderSelection(QObject *model);
    // Desktop containment on the anchor's screen and current activity.
    Plasma::Containment *desktopContainment() const;
    // The same, but only when it is a folder view ("Icons" desktop) —
    // the only containment with file-management contextual actions.
    Plasma::Containment *folderViewContainment() const;
    // The folder view's FolderModel instance behind the containment:
    // KNewFileMenu is created with the model as its QObject parent, so
    // the borrowed "newMenu" action leads straight to it.
    QObject *folderModel(Plasma::Containment *folder);
    // FolderModel::action(name), a Q_INVOKABLE returning the model's
    // named shared action (cut, copy, paste, trash, …).
    QAction *modelAction(QObject *model, const QString &name);
    // FolderModel::hasSelection(): gates the selection-dependent file
    // operations, which stay greyed out until desktop icons are selected.
    bool folderHasSelection(QObject *model) const;
    // False while the dropdown's actions are borrowed by QuickBar's open
    // popup; the rebuild is postponed until flushPendingRebuild().
    bool canRebuildDropdown(QMenu *menu, int lastBuiltCount);
    void rebuildFileMenu();
    void rebuildEditMenu();
    void rebuildViewMenu();
    void rebuildPlacesMenu();
    // Live proxy of a folder-view view-options action for the View
    // dropdown: the source stays in Plasma's own "Icons" submenu (which
    // must not be emptied), the clone is what QuickBar shows and
    // borrows. State mirrors from the source, triggering the clone
    // forwards to the source. Recursive for submenus.
    QAction *proxyViewAction(QAction *source, QMenu *owner);
    void syncProxyAction(const QAction *source, QAction *proxy);
    // Resolves the model row for a place URL captured at build time.
    QModelIndex placeIndexForUrl(const QUrl &url) const;
    void openPlace(const QUrl &url);

    std::unique_ptr<QMenu> m_menu;
    QPointer<QMenu> m_fileMenu;
    QPointer<QMenu> m_editMenu;
    QPointer<QMenu> m_viewMenu;
    QPointer<QMenu> m_placesMenu;
    // Persistent submenus for the remote ("Network") and tag ("Tags")
    // place groups. Created once: rebuilds detach and re-attach them
    // (QMenu::clear() removes but does not delete addMenu()-created
    // submenus), so they never leak or duplicate.
    QPointer<QMenu> m_networkMenu;
    QPointer<QMenu> m_tagsMenu;
    QPointer<KFilePlacesModel> m_placesModel;
    QPointer<Plasma::Containment> m_anchorContainment;
    // FolderModel behind the current desktop containment, re-resolved on
    // every rebuild; kept for the Properties action's late trigger and
    // the live selectionChanged() subscription.
    QPointer<QObject> m_folderModel;
    // Selection-dependent borrowed actions, re-gated live.
    QPointer<QAction> m_trashAction;
    QPointer<QAction> m_delAction;
    QPointer<QAction> m_renameAction;
    QPointer<QAction> m_cutAction;
    QPointer<QAction> m_copyAction;
    // True while this object emits the containment's
    // contextualActionsAboutToShow (whose Plasma-side handler chain ends
    // with clearSelection()).
    bool m_emittingContextMenu = false;
    // Long-lived activities service handle: current activity for the
    // desktop-containment lookup and the activity count that decides
    // whether "Show Activity Switcher" is offered, like Plasma's own
    // desktop context menu.
    KActivities::Consumer *m_activitiesConsumer = nullptr;
    // "Create New": QuickBar's own action so it can carry this domain's
    // label (KIO's KNewFileMenu translation reads "Create New", not
    // "New"); opens the real KNewFileMenu submenu, whose enabled state
    // is mirrored from Plasma's action.
    QAction *m_newAction = nullptr;
    // "Properties" for the desktop selection. FolderModel has no shared
    // properties action (Plasma builds one per popup), so this calls the
    // model's Q_INVOKABLE openPropertiesDialog() instead.
    QAction *m_propertiesAction = nullptr;
    // "Display Configuration" entry of the Edit dropdown, created once
    // and shown only while kscreen's KCM is available.
    QAction *m_displayConfigurationAction = nullptr;
    // Number of actions added by the last rebuild, per dropdown. Used to
    // tell an empty model from a dropdown whose actions are borrowed by
    // QuickBar.
    int m_lastBuiltCount = 0;
    int m_lastFileBuiltCount = 0;
    int m_lastEditBuiltCount = 0;
    int m_lastViewBuiltCount = 0;
    // Place mounted on request from this menu; setupDone opens it after.
    QUrl m_pendingSetupUrl;
    bool m_rebuildPending = false;
};
