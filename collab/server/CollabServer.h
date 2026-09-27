/*
 * CollabServer.h - collaboration server: shared projects, clients and transaction ordering
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

#ifndef LMMS_COLLAB_SERVER_H
#define LMMS_COLLAB_SERVER_H

#include <map>
#include <memory>

#include <QDir>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "CollabProtocol.h"
#include "ProjectState.h"

class QTcpServer;
class QTcpSocket;

namespace lmms::collab
{

class CollabServer : public QObject
{
	Q_OBJECT
public:
	CollabServer(const QDir& dataDir, QObject* parent = nullptr);
	~CollabServer() override;

	bool listen(const QString& address, quint16 port);
	//! Writes every modified project to disk (also called periodically and on shutdown)
	void saveAll();

	//! Project names are used as directory names, so they are restricted to a safe character set
	static bool isValidProjectName(const QString& name);

private:
	struct Project
	{
		QString name;
		ProjectState state;
		qint64 seq = 0;
		bool dirty = false;
	};

	struct Client
	{
		QTcpSocket* socket = nullptr;
		proto::FrameDecoder decoder;
		QString clientId;
		QString user;
		QString color = "#cccccc";
		Project* project = nullptr;
		QJsonObject presence;          //!< last presence, for users who join later (never stored on disk)
		QElapsedTimer presenceForwarded;
		bool presencePending = false;
	};

	void onNewConnection();
	void onReadyRead(QTcpSocket* socket);
	void onDisconnected(QTcpSocket* socket);
	void handleMessage(Client& client, const QJsonObject& message);
	void handleCreate(Client& client, const QJsonObject& message);
	void handleOpen(Client& client, const QJsonObject& message);
	void handleTx(Client& client, const QJsonObject& message);
	void handlePresence(Client& client, const QJsonObject& message);
	//! Sends the presence of everybody else in the client's project to a client that just joined
	void sendPresenceOfOthers(Client& client);

	void send(Client& client, const QJsonObject& message);
	void sendError(Client& client, const QString& text);
	void broadcast(Project* project, const QJsonObject& message, const Client* except = nullptr);

	Project* findOrLoadProject(const QString& name);
	QString projectDir(const QString& name) const;
	bool saveProject(Project& project);

	QDir m_dataDir;
	QTcpServer* m_server;
	QTimer m_saveTimer;
	std::map<QTcpSocket*, Client> m_clients;
	std::map<QString, std::unique_ptr<Project>> m_projects;
	int m_nextClientNumber = 1;
};

} // namespace lmms::collab

#endif // LMMS_COLLAB_SERVER_H
