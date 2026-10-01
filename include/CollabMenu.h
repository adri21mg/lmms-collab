/*
 * CollabMenu.h - "Collaboration" menu of the main window
 *
 * Copyright (c) 2026 adri21mg
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef LMMS_GUI_COLLAB_MENU_H
#define LMMS_GUI_COLLAB_MENU_H

#include <QMenu>
#include <QPointer>
#include <QSet>

class QProcess;
class QDialog;
class QTreeWidget;

namespace lmms::gui
{

class MainWindow;

class CollabMenu : public QMenu
{
	Q_OBJECT
public:
	explicit CollabMenu(MainWindow* mainWindow);

private:
	void showConnectDialog();
	void updateState();
	//! "Host a session": the collaboration server, run by this LMMS for as long as it is open; error text or ""
	QString startHosting();
	void stopHosting();
	//! "Create version...": asks for a description, the server keeps the project as it is now
	void createVersion();
	//! "Versions...": the project's versions (kept open and updated while someone creates one)
	void showVersions();
	void fillVersions(const QJsonArray& versions);
	bool isHosting() const;
	//! Addresses of this computer others can connect to (Tailscale first)
	static QString hostAddresses();

	MainWindow* m_mainWindow;
	QAction* m_connectAction;
	QAction* m_disconnectAction;
	QAction* m_saveAction;
	QAction* m_stopHostingAction;
	QAction* m_createVersionAction;
	QAction* m_versionsAction;
	QPointer<QDialog> m_versionsDialog;
	QTreeWidget* m_versionsList = nullptr;
	QSet<int> m_myVersions; //!< versions this user created in this session (their Perforce errors are shown)
	QProcess* m_hostServer = nullptr;
	QAction* m_statusAction;
};

} // namespace lmms::gui

#endif // LMMS_GUI_COLLAB_MENU_H
