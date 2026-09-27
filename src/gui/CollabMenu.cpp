/*
 * CollabMenu.cpp - "Collaboration" menu of the main window
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

#include "CollabMenu.h"

#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenuBar>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "CollabPresence.h"
#include "CollabSession.h"
#include "ConfigManager.h"
#include "MainWindow.h"

namespace lmms::gui
{

namespace
{
constexpr auto ConfigSection = "collab";
}

using collab::CollabSession;


CollabMenu::CollabMenu(MainWindow* mainWindow) :
	QMenu(tr("&Collaboration"), mainWindow),
	m_mainWindow(mainWindow)
{
	m_connectAction = addAction(tr("Connect..."), this, &CollabMenu::showConnectDialog);
	m_disconnectAction = addAction(tr("Disconnect"), this, [] { CollabSession::instance()->disconnectFromServer(); });
	addSeparator();

	// Collaborators' cursors and where they are (shown at the right of the menu bar)
	auto presence = new CollabPresence(mainWindow);
	mainWindow->menuBar()->setCornerWidget(new CollabPresenceBar(presence, mainWindow->menuBar()), Qt::TopRightCorner);
	auto playheads = addAction(tr("Show collaborators' playback position"));
	playheads->setCheckable(true);
	playheads->setChecked(presence->showPlayheads());
	connect(playheads, &QAction::toggled, presence, &CollabPresence::setShowPlayheads);
	addSeparator();

	m_statusAction = addAction(QString{});
	m_statusAction->setEnabled(false);

	auto session = CollabSession::instance();
	connect(session, &CollabSession::stateChanged, this, &CollabMenu::updateState);
	connect(session, &CollabSession::errorOccurred, this, [this](const QString& message) {
		QMessageBox::warning(m_mainWindow, tr("Collaboration"), message);
	});
	updateState();
}


void CollabMenu::updateState()
{
	const auto session = CollabSession::instance();
	QString status;
	switch (session->state())
	{
	case CollabSession::State::Disconnected: status = tr("Not connected"); break;
	case CollabSession::State::Connecting: status = tr("Connecting..."); break;
	case CollabSession::State::Joining: status = tr("Joining \"%1\"...").arg(session->projectName()); break;
	case CollabSession::State::Live:
		status = tr("Connected to \"%1\" as %2").arg(session->projectName(), session->userName());
		break;
	}
	m_statusAction->setText(status);
	m_connectAction->setEnabled(session->state() == CollabSession::State::Disconnected);
	m_disconnectAction->setEnabled(session->state() != CollabSession::State::Disconnected);
}


void CollabMenu::showConnectDialog()
{
	auto config = ConfigManager::inst();
	auto valueOr = [config](const char* key, const QString& fallback) {
		const QString v = config->value(ConfigSection, key);
		return v.isEmpty() ? fallback : v;
	};

	QDialog dialog{m_mainWindow};
	dialog.setWindowTitle(tr("Connect to collaboration server"));
	auto server = new QLineEdit{valueOr("server", QString{"127.0.0.1:%1"}.arg(collab::proto::DefaultPort))};
	auto user = new QLineEdit{valueOr("user", QString{})};
	auto project = new QLineEdit{valueOr("project", "m1-test")};
	auto openMode = new QRadioButton{tr("Open an existing shared project (replaces the current song)")};
	auto createMode = new QRadioButton{tr("Share my current song as a new shared project")};
	(config->value(ConfigSection, "mode") == "create" ? createMode : openMode)->setChecked(true);

	// Color of this user's cursor for the others: chosen once, otherwise derived from the name
	QColor color{config->value(ConfigSection, "color")};
	bool colorChosen = color.isValid();
	auto colorButton = new QPushButton;
	colorButton->setToolTip(tr("Color of your cursor for the other collaborators"));
	auto showColor = [&] {
		const QColor c = colorChosen ? color : CollabPresence::defaultColor(user->text());
		QPixmap swatch{40, 14};
		swatch.fill(c);
		colorButton->setIcon(QIcon{swatch});
		colorButton->setIconSize(swatch.size());
	};
	connect(user, &QLineEdit::textChanged, &dialog, [&] { showColor(); });
	connect(colorButton, &QPushButton::clicked, &dialog, [&] {
		const QColor picked = QColorDialog::getColor(colorChosen ? color : CollabPresence::defaultColor(user->text()),
			&dialog, tr("Your color"));
		if (!picked.isValid()) { return; }
		color = picked;
		colorChosen = true;
		showColor();
	});
	showColor();

	auto form = new QFormLayout;
	form->addRow(tr("Server:"), server);
	form->addRow(tr("Your name:"), user);
	form->addRow(tr("Your color:"), colorButton);
	form->addRow(tr("Project:"), project);
	auto buttons = new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel};
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	auto layout = new QVBoxLayout{&dialog};
	layout->addLayout(form);
	layout->addWidget(openMode);
	layout->addWidget(createMode);
	layout->addWidget(buttons);

	while (dialog.exec() == QDialog::Accepted)
	{
		const QString address = server->text().trimmed();
		const int colon = address.lastIndexOf(':');
		bool portOk = false;
		const uint port = colon > 0 ? address.mid(colon + 1).toUInt(&portOk) : 0;
		if (!portOk || port == 0 || port > 65535)
		{
			QMessageBox::warning(&dialog, dialog.windowTitle(), tr("Use the form host:port for the server."));
			continue;
		}
		if (user->text().trimmed().isEmpty() || project->text().trimmed().isEmpty())
		{
			QMessageBox::warning(&dialog, dialog.windowTitle(), tr("Please enter your name and a project name."));
			continue;
		}

		const bool create = createMode->isChecked();
		config->setValue(ConfigSection, "server", address);
		config->setValue(ConfigSection, "user", user->text().trimmed());
		config->setValue(ConfigSection, "project", project->text().trimmed());
		config->setValue(ConfigSection, "mode", create ? "create" : "open");
		if (colorChosen) { config->setValue(ConfigSection, "color", color.name()); }
		const QColor userColor = colorChosen ? color : CollabPresence::defaultColor(user->text());

		// Opening a shared project replaces the current song, so offer to save it first
		if (!create && !m_mainWindow->mayChangeProject(true)) { return; }

		CollabSession::instance()->connectToServer(address.left(colon), static_cast<quint16>(port),
			user->text().trimmed(), project->text().trimmed(),
			create ? CollabSession::JoinMode::Create : CollabSession::JoinMode::Open, userColor.name());
		return;
	}
}

} // namespace lmms::gui
