/*
    SPDX-FileCopyrightText: 2026 QuickBar contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "genericmenu.h"

#include "quickbaractions.h"

#include <KLocalizedString>
#include <QAction>
#include <QKeySequence>
#include <QMenu>

namespace
{
QAction *addAction(QMenu *menu, const QString &text, QKeySequence::StandardKey key)
{
    auto *action = menu->addAction(text);
    action->setShortcut(QKeySequence(key));
    return action;
}
} // namespace

QMenu *GenericMenu::create(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("quickbar-generic-menu"));

    auto *editMenu = menu->addMenu(i18n("&Edit"));
    addAction(editMenu, i18n("&Undo"), QKeySequence::Undo);
    addAction(editMenu, i18n("&Redo"), QKeySequence::Redo);
    editMenu->addSeparator();
    addAction(editMenu, i18n("Cu&t"), QKeySequence::Cut);
    addAction(editMenu, i18n("&Copy"), QKeySequence::Copy);
    addAction(editMenu, i18n("&Paste"), QKeySequence::Paste);
    editMenu->addSeparator();
    addAction(editMenu, i18n("Select &All"), QKeySequence::SelectAll);

    auto *helpMenu = menu->addMenu(i18n("&Help"));
    auto *aboutAction = helpMenu->addAction(i18n("&About"));
    aboutAction->setProperty(QuickBarActions::property, QLatin1String(QuickBarActions::about));

    return menu;
}
