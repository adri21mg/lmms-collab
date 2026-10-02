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

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <thread>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QTimer>

#include "CollabProtocol.h"
#include "P4Exporter.h"
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
	//! Versions are also submitted to Perforce
	void setPerforce(const P4Exporter::Config& config);

private:
	struct Project
	{
		struct Asset
		{
			QString path;  //!< inside the project's library folder ("shared:<path>" in the project)
			qint64 size = 0;
		};
		QString name;
		ProjectState state;
		qint64 seq = 0;
		qint64 savedSeq = 0; //!< transactions up to here are on disk
		bool dirty = false;
		QHash<QString, Asset> library; //!< shared files by SHA-256
	};

	//! A file being uploaded by a client
	struct Upload
	{
		QString name;
		qint64 size = 0;
		qint64 received = 0;
		std::unique_ptr<QFile> file;
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
		std::map<QString, Upload> uploads; //!< by SHA-256
	};

	void onNewConnection();
	void onReadyRead(QTcpSocket* socket);
	void onDisconnected(QTcpSocket* socket);
	void handleMessage(Client& client, const QJsonObject& message);
	void handleCreate(Client& client, const QJsonObject& message);
	void handleOpen(Client& client, const QJsonObject& message);
	void handleTx(Client& client, const QJsonObject& message);
	void handlePresence(Client& client, const QJsonObject& message);
	void handleList(Client& client);
	void handleVersionCreate(Client& client, const QJsonObject& message);
	void handleVersionsGet(Client& client);
	void handleVersionRestore(Client& client, const QJsonObject& message);
	//! A version of the project as it is now (also submitted to Perforce); nullopt with @p error if it failed
	std::optional<QJsonObject> createVersion(Project& project, const Client& client, const QString& description,
		QString& error);
	//! Versions of a project, oldest first
	QJsonArray versionList(const QString& project) const;
	bool writeVersion(const QString& project, const QJsonObject& version);
	//! Starts the next Perforce submit, one at a time, on a worker thread
	void startP4Job();
	void p4JobDone(const QString& project, int id, const P4Exporter::Result& result);
	void handleAssetPut(Client& client, const QJsonObject& message);
	void handleAssetGet(Client& client, const QJsonObject& message);
	//! A part of an uploaded file
	void handleBinary(Client& client, const QByteArray& payload);
	void dropUploads(Client& client);
	//! The shared files as sent in joined
	static QJsonArray libraryList(const Project& project);
	bool saveLibrary(const Project& project);
	//! Sends the presence of everybody else in the client's project to a client that just joined
	void sendPresenceOfOthers(Client& client);

	void send(Client& client, const QJsonObject& message);
	void sendError(Client& client, const QString& text);
	void broadcast(Project* project, const QJsonObject& message, const Client* except = nullptr);

	Project* findOrLoadProject(const QString& name);
	QString projectDir(const QString& name) const;
	bool saveProject(Project& project);
	//! Tells the project's clients that it is on disk up to its current transaction
	void announceSaved(Project& project);

	QDir m_dataDir;
	QTcpServer* m_server;
	QTimer m_saveTimer;
	std::map<QTcpSocket*, Client> m_clients;
	std::map<QString, std::unique_ptr<Project>> m_projects;
	int m_nextClientNumber = 1;

	struct P4Job
	{
		int id = 0;
		P4Exporter::Job job;
	};
	std::optional<P4Exporter::Config> m_p4;
	std::deque<P4Job> m_p4Queue;
	std::thread m_p4Thread;
	bool m_p4Busy = false;
};

} // namespace lmms::collab

#endif // LMMS_COLLAB_SERVER_H
