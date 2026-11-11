/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "desktopmenu.h"

#include <KAuthorized>
#include <KDialogJobUiDelegate>
#include <KFilePlacesModel>
#include <KIO/ApplicationLauncherJob>
#include <KIO/FileUndoManager>
#include <KIO/OpenUrlJob>
#include <KIO/StatJob>
#include <KLocalizedString>
#include <KService>
#include <KStandardAction>

#include <QAbstractItemModel>
#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QProcess>
#include <QQmlListProperty>

#include <memory>

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
    // The dropdown's natural width sits right at the style's elision
    // threshold, cropping longer entries ("Open File Manager"). Keep it a
    // bit wider so every label fits.
    m_fileMenu->setMinimumWidth(240);
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

    // Rename is QuickBar's own action instead of the borrowed FolderModel
    // one, so Plasma's updateActions() can't re-enable it while nothing
    // is selected; see the header for details. The standard action look
    // matches Plasma's own rename, which fires the very same
    // requestRename() signal.
    m_renameAction = KStandardAction::renameFile(nullptr, nullptr, this);
    connect(m_renameAction, &QAction::triggered, this, [this]() {
        if (m_folderModel) {
            // FolderModel::requestRename(): the folder view opens its
            // inline editor for the current selection.
            QMetaObject::invokeMethod(m_folderModel, "requestRename");
        }
    });

    // Open File Manager opens Dolphin at its default location: the same
    // service launch the Meta+E shortcut performs (Dolphin's desktop
    // file is what declares that shortcut).
    m_newWindowAction = new QAction(QIcon::fromTheme(QStringLiteral("system-file-manager")), i18n("Open File &Manager"), this);
    connect(m_newWindowAction, &QAction::triggered, this, [] {
        if (auto service = KService::serviceByStorageId(QStringLiteral("org.kde.dolphin"))) {
            auto *job = new KIO::ApplicationLauncherJob(service);
            job->start();
        }
    });

    // Open Terminal sits right below Open File Manager, launched through
    // Konsole's service the same way (no new dependency).
    m_terminalAction = new QAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")), i18n("Open &Terminal"), this);
    connect(m_terminalAction, &QAction::triggered, this, [] {
        if (auto service = KService::serviceByStorageId(QStringLiteral("org.kde.konsole"))) {
            auto *job = new KIO::ApplicationLauncherJob(service);
            job->start();
        }
    });

    // Compress submenu, mirroring ark's own folder-view dropdown, entry
    // order and labels ("Compress to «tar.gz»" style, from ark's own
    // msgid). URLs are collected at trigger time (selections happen
    // after the last rebuild).
    m_compressMenu = m_fileMenu->addMenu(i18n("&Compress"));
    m_compressMenu->setIcon(QIcon::fromTheme(QStringLiteral("archive-insert")));
    m_compressAction = m_compressMenu->menuAction();
    QAction *compressTarGzAction = m_compressMenu->addAction(QIcon::fromTheme(QStringLiteral("application-x-compressed-tar")),
                                                             i18n("Compress to \"%1\"", QStringLiteral("tar.gz")));
    connect(compressTarGzAction, &QAction::triggered, this, [this]() {
        compressSelection(QStringLiteral("tar.gz"));
    });
    QAction *compressZipAction = m_compressMenu->addAction(QIcon::fromTheme(QStringLiteral("application-zip")),
                                                           i18n("Compress to \"%1\"", QStringLiteral("zip")));
    connect(compressZipAction, &QAction::triggered, this, [this]() {
        compressSelection(QStringLiteral("zip"));
    });
    QAction *compressToAction = m_compressMenu->addAction(QIcon::fromTheme(QStringLiteral("archive-insert")), i18n("Compress to…"));
    connect(compressToAction, &QAction::triggered, this, [this]() {
        compressSelection(QString());
    });

    // Select All: FolderModel::setRangeSelected() over every row; the
    // selectionChanged() it fires re-enables the selection-dependent
    // entries instantly through the live gate.
    m_selectAllAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-select-all")), i18n("Select &All"), this);
    connect(m_selectAllAction, &QAction::triggered, this, [this]() {
        const auto *model = qobject_cast<const QAbstractItemModel *>(m_folderModel.data());
        const int count = model ? model->rowCount() : 0;
        if (count > 0) {
            QMetaObject::invokeMethod(m_folderModel, "setRangeSelected", Q_ARG(int, 0), Q_ARG(int, count - 1));
        }
    });

    // Cut is QuickBar's own action instead of the borrowed FolderModel
    // one, for the same reason as Rename (Plasma's updateActions()
    // re-enables the borrowed one with nothing selected). The standard
    // action look matches Plasma's own cut, which calls the model's
    // cut().
    m_cutAction = KStandardAction::cut(nullptr, nullptr, this);
    connect(m_cutAction, &QAction::triggered, this, [this]() {
        if (m_folderModel) {
            // FolderModel::cut(): the selection to the clipboard with
            // the cut marker, exactly what Plasma's own action calls.
            QMetaObject::invokeMethod(m_folderModel, "cut");
        }
    });

    // Redo, mirroring FolderModel's own undo wiring: the standard
    // action's text and icon, enabled state and description text
    // straight from KIO's global FileUndoManager.
    KIO::FileUndoManager *undoManager = KIO::FileUndoManager::self();
    m_redoAction = KStandardAction::redo(undoManager, &KIO::FileUndoManager::redo, this);
    m_redoAction->setEnabled(undoManager->isRedoAvailable());
    connect(undoManager, &KIO::FileUndoManager::redoAvailable, m_redoAction, &QAction::setEnabled);
    connect(undoManager, &KIO::FileUndoManager::redoTextChanged, this, [this](const QString &text) {
        if (!text.isEmpty()) {
            m_redoAction->setText(text);
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
    // At login the FolderView QML pushes its shared actions (newMenu and
    // the FolderModel behind it) into the containment's contextual
    // actions only after the first refresh has run, which leaves File
    // and View hidden until the next desktop activation rebuilds them.
    // Rebuild again as soon as they arrive.
    if (Plasma::Containment *desktop = desktopContainment()) {
        connect(desktop, &Plasma::Applet::contextualActionsChanged, this, &DesktopMenu::refreshDesktopActions, Qt::UniqueConnection);
    }

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
        m_copyAction.clear();
        m_openAction.clear();
    }
    if (model) {
        m_folderModel = model;
        // Selections are made on the desktop long after the last rebuild,
        // so keep the selection-dependent entries' enabled state live.
        connect(m_folderModel, SIGNAL(selectionChanged()), this, SLOT(onFolderSelectionChanged()));
    }
}

void DesktopMenu::compressSelection(const QString &suffix)
{
    // Ark's CLI: --batch --add --autofilename <suffix> creates
    // "<name>.<suffix>" next to the selection without any dialog, the
    // same archive creation its folder-view plugin performs; an empty
    // suffix (plain --add --dialog) opens ark's add dialog asking for
    // the archive name.
    const QList<QUrl> urls = selectedUrls(m_folderModel);
    if (urls.isEmpty()) {
        return;
    }

    // The desktop containment reports its items with desktop:/ URLs
    // (kio-desktop), not file:/. ark's batch interface resolves its
    // inputs with QUrl::toLocalFile(), so resolve every URL to its most
    // local form first — KIO::mostLocalUrl() asks the worker behind the
    // URL for the local path it serves, kio-desktop here.
    const auto resolvedFiles = std::make_shared<QStringList>();
    const auto pending = std::make_shared<int>(urls.count());
    for (const QUrl &url : urls) {
        KIO::StatJob *job = KIO::mostLocalUrl(url);
        connect(job, &KJob::result, this, [this, suffix, resolvedFiles, pending](KJob *finishedJob) {
            const auto *statJob = static_cast<KIO::StatJob *>(finishedJob);
            const QUrl localUrl = statJob->mostLocalUrl();
            if (localUrl.isLocalFile() && !localUrl.toLocalFile().isEmpty()) {
                resolvedFiles->append(localUrl.toLocalFile());
            }
            if (--(*pending) == 0) {
                launchArk(suffix, *resolvedFiles);
            }
        });
    }
}

void DesktopMenu::launchArk(const QString &suffix, const QStringList &files)
{
    if (files.isEmpty()) {
        return;
    }
    QStringList args{QStringLiteral("--add")};
    if (!suffix.isEmpty()) {
        args << QStringLiteral("--batch") << QStringLiteral("--autofilename") << suffix;
    } else {
        args << QStringLiteral("--dialog");
    }
    args << files;
    QProcess::startDetached(QStringLiteral("ark"), args);
}

void DesktopMenu::onFolderSelectionChanged()
{
    if (!m_folderModel || m_emittingContextMenu) {
        return;
    }
    const bool hasSelection = folderHasSelection(m_folderModel);
    const QPointer<QAction> gated[] = {m_openAction, m_trashAction, m_delAction, m_renameAction, m_compressAction, m_cutAction, m_copyAction};
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

QList<QUrl> DesktopMenu::selectedUrls(QObject *model) const
{
    // Read through the model's own "selected" and "url" roles; the
    // selection lives in FolderModel's private QItemSelectionModel, which
    // is only exposed that way.
    QList<QUrl> urls;
    const auto *itemModel = qobject_cast<const QAbstractItemModel *>(model);
    if (!itemModel) {
        return urls;
    }
    const QHash<int, QByteArray> roles = itemModel->roleNames();
    const int selectedRole = roles.key(QByteArrayLiteral("selected"), -1);
    const int urlRole = roles.key(QByteArrayLiteral("url"), -1);
    if (selectedRole < 0 || urlRole < 0) {
        return urls;
    }
    const int rows = itemModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = itemModel->index(row, 0);
        if (index.data(selectedRole).toBool()) {
            const QUrl url = index.data(urlRole).toUrl();
            // Skip empty roles (blank or drag rows); every well-formed
            // URL is kept — the desktop containment reports desktop:/
            // URLs, which compressSelection() resolves to local paths
            // through KIO before handing them to ark.
            if (!url.isValid() || url.isEmpty()) {
                continue;
            }
            urls.append(url);
        }
    }
    return urls;
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

    // New Window, Finder's "New Finder Window": Dolphin at its default
    // location, the same service launch as the Meta+E shortcut. Open
    // Terminal follows it.
    m_fileMenu->addAction(m_newWindowAction);
    ++built;
    m_fileMenu->addAction(m_terminalAction);
    ++built;
    m_fileMenu->addSeparator();

    // Open, Rename and Compress, for the desktop selection. Rename is
    // QuickBar's own action, so its greyed-out state survives Plasma's
    // updateActions() runs (see the header).
    m_openAction = modelAction(model, QStringLiteral("open"));
    if (m_openAction) {
        m_openAction->setEnabled(hasSelection);
        m_fileMenu->addAction(m_openAction);
        ++built;
    }
    m_renameAction->setEnabled(hasSelection);
    m_fileMenu->addAction(m_renameAction);
    ++built;
    m_compressAction->setEnabled(hasSelection);
    m_fileMenu->addMenu(m_compressMenu);
    ++built;
    m_fileMenu->addSeparator();

    // Move to Trash and Delete, for the desktop selection.
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
        // Cut is QuickBar's own action, like Rename in the File dropdown.
        const QList<QString> names = {QStringLiteral("undo"), QStringLiteral("cut"), QStringLiteral("copy"), QStringLiteral("paste")};
        for (const QString &name : names) {
            if (name == QLatin1String("cut")) {
                m_cutAction->setEnabled(hasSelection);
                m_editMenu->addAction(m_cutAction);
                ++built;
                continue;
            }
            QAction *action = modelAction(model, name);
            if (!action) {
                continue;
            }
            if (name == QLatin1String("copy")) {
                action->setEnabled(hasSelection);
                m_copyAction = action;
            }
            m_editMenu->addAction(action);
            ++built;
            if (name == QLatin1String("undo")) {
                // Redo next to Undo, wired like Plasma's own undo.
                m_editMenu->addAction(m_redoAction);
                ++built;
            }
        }

        // Select All, Finder's Edit menu: selects every desktop icon;
        // the live gate turns the selection-dependent entries on.
        m_editMenu->addAction(m_selectAllAction);
        ++built;
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
