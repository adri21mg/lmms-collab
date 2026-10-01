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
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonArray>
#include <QListWidget>
#include <QNetworkInterface>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
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
#include "CollabProtocol.h"
#include "ConfigManager.h"
#include "lmmsconfig.h"
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
	m_disconnectAction = addAction(tr("Disconnect"), this, [] { CollabSession::instance()->leave(); });
	m_saveAction = addAction(tr("Save on server now"), this, [] { CollabSession::instance()->saveNow(); });
	m_stopHostingAction = addAction(tr("Stop hosting the session"), this, &CollabMenu::stopHosting);
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
	if (session->isReconnecting())
	{
		status = tr("Connection lost: reconnecting to \"%1\"...").arg(session->projectName());
	}
	else switch (session->state())
	{
	case CollabSession::State::Disconnected: status = tr("Not connected"); break;
	case CollabSession::State::Connecting: status = tr("Connecting..."); break;
	case CollabSession::State::Joining: status = tr("Joining \"%1\"...").arg(session->projectName()); break;
	case CollabSession::State::Live:
		status = tr("Connected to \"%1\" as %2").arg(session->projectName(), session->userName());
		break;
	case CollabSession::State::Reconnecting:
		status = tr("Connection lost: reconnecting to \"%1\"...").arg(session->projectName());
		break;
	}
	m_statusAction->setText(status);
	const bool idle = session->state() == CollabSession::State::Disconnected && !session->isReconnecting();
	m_connectAction->setEnabled(idle);
	m_disconnectAction->setEnabled(!idle); // also stops reconnecting
	m_saveAction->setEnabled(session->state() == CollabSession::State::Live);
	m_stopHostingAction->setVisible(isHosting());
}


//! Asks a server for its projects with a short connection of its own (hello, list)
class ProjectLister : public QObject
{
	Q_OBJECT
public:
	ProjectLister(const QString& host, quint16 port, const QString& user, QObject* parent) : QObject(parent)
	{
		connect(&m_socket, &QTcpSocket::connected, this, [this, user] {
			m_socket.write(collab::proto::encodeJsonFrame({{"t", collab::proto::msg::Hello},
				{"proto", collab::proto::Version}, {"user", user.isEmpty() ? QString{"?"} : user}}));
			m_socket.write(collab::proto::encodeJsonFrame({{"t", collab::proto::msg::List}}));
		});
		connect(&m_socket, &QTcpSocket::readyRead, this, [this] {
			m_decoder.append(m_socket.readAll());
			collab::proto::FrameType type;
			QByteArray payload;
			while (m_decoder.next(type, payload))
			{
				const auto message = collab::proto::parseMessage(payload);
				if (!message) { continue; }
				const QString t = message->value("t").toString();
				if (t == collab::proto::msg::Projects) { return finish(message->value("projects").toArray(), {}); }
				if (t == collab::proto::msg::Error) { return finish({}, message->value("message").toString()); }
			}
		});
		connect(&m_socket, &QTcpSocket::errorOccurred, this, [this] { finish({}, m_socket.errorString()); });
		QTimer::singleShot(5000, this, [this] { finish({}, tr("The server does not answer.")); });
		m_socket.connectToHost(host, port);
	}

signals:
	void finished(const QJsonArray& projects, const QString& error);

private:
	void finish(const QJsonArray& projects, const QString& error)
	{
		if (m_done) { return; }
		m_done = true;
		m_socket.abort();
		emit finished(projects, error);
		deleteLater();
	}

	QTcpSocket m_socket;
	collab::proto::FrameDecoder m_decoder;
	bool m_done = false;
};


bool CollabMenu::isHosting() const
{
	return m_hostServer && m_hostServer->state() != QProcess::NotRunning;
}


QString CollabMenu::startHosting()
{
	if (isHosting()) { return {}; }
	// The server comes with LMMS; its projects are kept in this installation's workspace
#ifdef LMMS_BUILD_WIN32
	const QString program = QCoreApplication::applicationDirPath() + "/lmms-collab-server.exe";
#else
	const QString program = QCoreApplication::applicationDirPath() + "/lmms-collab-server";
#endif
	if (!QFileInfo::exists(program)) { return tr("The collaboration server was not found next to LMMS."); }
	m_hostServer = new QProcess(this);
	m_hostServer->setProgram(program);
	m_hostServer->setArguments({"--listen", "0.0.0.0", "--port", QString::number(collab::proto::DefaultPort),
		"--data", ConfigManager::inst()->workingDir() + "collab-host"});
	m_hostServer->setProcessChannelMode(QProcess::MergedChannels);
	m_hostServer->start();
	if (!m_hostServer->waitForStarted(3000)) { return tr("The collaboration server could not be started."); }
	QThread::msleep(300); // until it listens (or fails, e.g. the port is in use)
	if (m_hostServer->state() == QProcess::NotRunning)
	{
		return tr("The collaboration server stopped: %1").arg(QString::fromLocal8Bit(m_hostServer->readAll()).trimmed());
	}
	updateState();
	return {};
}


void CollabMenu::stopHosting()
{
	if (!isHosting()) { return; }
	// It saves every few seconds; ask for a last save first in case we are in one of its projects
	CollabSession::instance()->saveNow();
	QThread::msleep(300);
	m_hostServer->kill();
	m_hostServer->waitForFinished(2000);
	updateState();
}


QString CollabMenu::hostAddresses()
{
	// Addresses others may use: Tailscale (100.64.0.0/10) first, then the local network
	QStringList tailscale, local;
	for (const QHostAddress& address : QNetworkInterface::allAddresses())
	{
		if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) { continue; }
		const quint32 ip = address.toIPv4Address();
		const QString text = QString{"%1:%2"}.arg(address.toString()).arg(collab::proto::DefaultPort);
		if ((ip & 0xffc00000u) == 0x64400000u) { tailscale.append(text + " (Tailscale)"); }
		else if (address.isPrivateUse()) { local.append(text + tr(" (local network)")); }
	}
	return (tailscale + local).join("\n");
}


void CollabMenu::showConnectDialog()
{
	auto config = ConfigManager::inst();
	auto valueOr = [config](const char* key, const QString& fallback) {
		const QString v = config->value(ConfigSection, key);
		return v.isEmpty() ? fallback : v;
	};

	QDialog dialog{m_mainWindow};
	dialog.setWindowTitle(tr("Collaboration"));
	dialog.setMinimumWidth(480);

	// Server: the last ones used
	auto server = new QComboBox;
	server->setEditable(true);
	QStringList servers = config->value(ConfigSection, "servers").split(';', Qt::SkipEmptyParts);
	const QString lastServer = valueOr("server", QString{"127.0.0.1:%1"}.arg(collab::proto::DefaultPort));
	servers.removeAll(lastServer);
	servers.prepend(lastServer);
	server->addItems(servers);
	auto hostButton = new QPushButton{tr("Host a session")};
	hostButton->setToolTip(tr("Starts a server on this computer, for as long as LMMS is open; others connect to it"));
	auto hostInfo = new QLabel;
	hostInfo->setWordWrap(true);
	hostInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);

	auto user = new QLineEdit{valueOr("user", QString{})};

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

	// The server's projects
	auto projects = new QListWidget;
	projects->setMinimumHeight(150);
	auto refresh = new QPushButton{tr("Refresh")};
	auto listInfo = new QLabel;
	listInfo->setWordWrap(true);
	auto openButton = new QPushButton{tr("Join selected project")};
	openButton->setToolTip(tr("Replaces your current song with the shared project"));
	openButton->setEnabled(false);
	auto createButton = new QPushButton{tr("Share my current song as a new project...")};

	auto hostAndPort = [&](QString& host, quint16& port) {
		const QString address = server->currentText().trimmed();
		const int colon = address.lastIndexOf(':');
		bool ok = false;
		const uint p = colon > 0 ? address.mid(colon + 1).toUInt(&ok) : 0;
		if (!ok || p == 0 || p > 65535) { return false; }
		host = address.left(colon);
		port = static_cast<quint16>(p);
		return true;
	};
	auto loadProjects = [&] {
		projects->clear();
		openButton->setEnabled(false);
		QString host;
		quint16 port = 0;
		if (!hostAndPort(host, port))
		{
			listInfo->setText(tr("Use the form host:port for the server."));
			return;
		}
		listInfo->setText(tr("Asking the server..."));
		auto lister = new ProjectLister(host, port, user->text().trimmed(), &dialog);
		connect(lister, &ProjectLister::finished, &dialog, [&](const QJsonArray& list, const QString& error) {
			if (!error.isEmpty())
			{
				listInfo->setText(tr("Cannot reach the server: %1").arg(error));
				return;
			}
			const QString dash = QString{"  "} + QChar{0x2014} + "  ";
			for (const QJsonValue& value : list)
			{
				const QJsonObject p = value.toObject();
				const QDateTime modified = QDateTime::fromString(p.value("modified").toString(), Qt::ISODate).toLocalTime();
				QString text = p.value("name").toString();
				if (modified.isValid()) { text += dash + tr("saved %1").arg(modified.toString("dd/MM/yyyy HH:mm")); }
				if (const int users = p.value("users").toInt(); users > 0) { text += dash + tr("%1 connected").arg(users); }
				auto item = new QListWidgetItem{text, projects};
				item->setData(Qt::UserRole, p.value("name").toString());
				if (item->data(Qt::UserRole).toString() == config->value(ConfigSection, "project")) { projects->setCurrentItem(item); }
			}
			listInfo->setText(list.isEmpty() ? tr("No projects on this server yet: share your current song as the first one.")
				: QString{});
		});
	};
	connect(refresh, &QPushButton::clicked, &dialog, loadProjects);
	connect(server, &QComboBox::activated, &dialog, loadProjects);
	connect(projects, &QListWidget::currentItemChanged, &dialog, [&] { openButton->setEnabled(projects->currentItem() != nullptr); });

	auto showHosting = [&] {
		const QString addresses = hostAddresses();
		hostInfo->setText(tr("You are hosting a session. Others connect to:\n%1\n\nAnyone who can reach this computer on "
			"that port can join (passwords will come later): Tailscale is the safe way to play with friends over the "
			"internet.").arg(addresses.isEmpty() ? tr("(no network address found)") : addresses));
		hostButton->setEnabled(false);
	};
	connect(hostButton, &QPushButton::clicked, &dialog, [&] {
		if (const QString error = startHosting(); !error.isEmpty())
		{
			QMessageBox::warning(&dialog, dialog.windowTitle(), error);
			return;
		}
		server->setEditText(QString{"127.0.0.1:%1"}.arg(collab::proto::DefaultPort));
		showHosting();
		loadProjects();
	});
	if (isHosting()) { showHosting(); }

	auto form = new QFormLayout;
	auto serverRow = new QHBoxLayout;
	serverRow->addWidget(server, 1);
	serverRow->addWidget(hostButton);
	form->addRow(tr("Server:"), serverRow);
	form->addRow(QString{}, hostInfo);
	form->addRow(tr("Your name:"), user);
	form->addRow(tr("Your color:"), colorButton);
	auto listHeader = new QHBoxLayout;
	listHeader->addWidget(new QLabel{tr("Projects on this server:")}, 1);
	listHeader->addWidget(refresh);
	auto actions = new QHBoxLayout;
	actions->addWidget(createButton);
	actions->addStretch(1);
	actions->addWidget(openButton);
	auto cancel = new QPushButton{tr("Cancel")};
	actions->addWidget(cancel);
	auto layout = new QVBoxLayout{&dialog};
	layout->addLayout(form);
	layout->addLayout(listHeader);
	layout->addWidget(projects);
	layout->addWidget(listInfo);
	layout->addLayout(actions);

	// The choice: join a project, or share the current song as a new one
	QString chosenProject;
	bool create = false;
	auto finish = [&](bool newProject) {
		if (user->text().trimmed().isEmpty())
		{
			QMessageBox::warning(&dialog, dialog.windowTitle(), tr("Please enter your name."));
			return;
		}
		if (newProject)
		{
			bool ok = false;
			const QString name = QInputDialog::getText(&dialog, tr("New shared project"),
				tr("Name of the project (letters, numbers, spaces, - and _):"), QLineEdit::Normal, QString{}, &ok).trimmed();
			if (!ok || name.isEmpty()) { return; }
			chosenProject = name;
		}
		else if (projects->currentItem()) { chosenProject = projects->currentItem()->data(Qt::UserRole).toString(); }
		else { return; }
		create = newProject;
		dialog.accept();
	};
	connect(openButton, &QPushButton::clicked, &dialog, [&] { finish(false); });
	connect(projects, &QListWidget::itemDoubleClicked, &dialog, [&] { finish(false); });
	connect(createButton, &QPushButton::clicked, &dialog, [&] { finish(true); });
	connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);

	QTimer::singleShot(0, &dialog, loadProjects);
	if (dialog.exec() != QDialog::Accepted) { return; }

	QString host;
	quint16 port = 0;
	if (!hostAndPort(host, port)) { return; }
	const QString address = server->currentText().trimmed();
	servers.removeAll(address);
	servers.prepend(address);
	while (servers.size() > 8) { servers.removeLast(); }
	config->setValue(ConfigSection, "servers", servers.join(';'));
	config->setValue(ConfigSection, "server", address);
	config->setValue(ConfigSection, "user", user->text().trimmed());
	config->setValue(ConfigSection, "project", chosenProject);
	if (colorChosen) { config->setValue(ConfigSection, "color", color.name()); }
	const QColor userColor = colorChosen ? color : CollabPresence::defaultColor(user->text());

	// Opening a shared project replaces the current song, so offer to save it first
	if (!create && !m_mainWindow->mayChangeProject(true)) { return; }

	CollabSession::instance()->connectToServer(host, port, user->text().trimmed(), chosenProject,
		create ? CollabSession::JoinMode::Create : CollabSession::JoinMode::Open, userColor.name());
}

} // namespace lmms::gui

#include "CollabMenu.moc"
