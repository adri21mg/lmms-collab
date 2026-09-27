/*
 * CollabServer.cpp - collaboration server: shared projects, clients and transaction ordering
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

#include "CollabServer.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>

namespace lmms::collab
{

namespace
{
constexpr int SaveIntervalMs = 5000;
const QString LiveFile = "live/project.mmp";
const QString ManifestFile = "manifest.json";
}


CollabServer::CollabServer(const QDir& dataDir, QObject* parent) :
	QObject(parent),
	m_dataDir(dataDir),
	m_server(new QTcpServer(this))
{
	connect(m_server, &QTcpServer::newConnection, this, &CollabServer::onNewConnection);
	connect(&m_saveTimer, &QTimer::timeout, this, &CollabServer::saveAll);
	m_saveTimer.start(SaveIntervalMs);
}


CollabServer::~CollabServer()
{
	saveAll();
}


bool CollabServer::listen(const QString& address, quint16 port)
{
	if (!m_server->listen(QHostAddress{address}, port))
	{
		qCritical("Cannot listen on %s:%u: %s", qPrintable(address), port, qPrintable(m_server->errorString()));
		return false;
	}
	qInfo("Listening on %s:%u, data directory %s", qPrintable(address), port,
		qPrintable(QDir::toNativeSeparators(m_dataDir.absolutePath())));
	return true;
}


bool CollabServer::isValidProjectName(const QString& name)
{
	static const QRegularExpression re{"^[A-Za-z0-9][A-Za-z0-9 _-]{0,63}$"};
	return re.match(name).hasMatch() && !name.endsWith(' ');
}


void CollabServer::onNewConnection()
{
	while (QTcpSocket* socket = m_server->nextPendingConnection())
	{
		Client& client = m_clients[socket];
		client.socket = socket;
		client.clientId = QString{"c%1"}.arg(m_nextClientNumber++);
		connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
		connect(socket, &QTcpSocket::disconnected, this, [this, socket] { onDisconnected(socket); });
		qInfo("[%s] connected from %s", qPrintable(client.clientId), qPrintable(socket->peerAddress().toString()));
	}
}


void CollabServer::onDisconnected(QTcpSocket* socket)
{
	const auto it = m_clients.find(socket);
	if (it == m_clients.end()) { return; }
	qInfo("[%s] %s disconnected", qPrintable(it->second.clientId), qPrintable(it->second.user));
	Project* project = it->second.project;
	const QString clientId = it->second.clientId;
	const bool hadPresence = !it->second.presence.isEmpty();
	m_clients.erase(it);
	if (project && hadPresence)
	{
		broadcast(project, {{"t", proto::msg::Presence}, {"clientId", clientId}, {"gone", true}});
	}
	socket->deleteLater();
}


void CollabServer::onReadyRead(QTcpSocket* socket)
{
	const auto it = m_clients.find(socket);
	if (it == m_clients.end()) { return; }
	Client& client = it->second;

	client.decoder.append(socket->readAll());
	proto::FrameType type;
	QByteArray payload;
	while (client.decoder.next(type, payload))
	{
		const auto message = type == proto::FrameType::Json ? proto::parseMessage(payload) : std::nullopt;
		if (!message)
		{
			qWarning("[%s] invalid frame, closing connection", qPrintable(client.clientId));
			socket->abort();
			return;
		}
		handleMessage(client, *message);
		if (m_clients.find(socket) == m_clients.end()) { return; } // connection dropped meanwhile
	}
	if (client.decoder.hasError())
	{
		qWarning("[%s] framing error, closing connection", qPrintable(client.clientId));
		socket->abort();
	}
}


void CollabServer::handleMessage(Client& client, const QJsonObject& message)
{
	const QString t = message.value("t").toString();
	if (t == proto::msg::Hello)
	{
		if (message.value("proto").toInt() != proto::Version)
		{
			sendError(client, QString{"protocol version mismatch (server %1)"}.arg(proto::Version));
			client.socket->disconnectFromHost();
			return;
		}
		client.user = message.value("user").toString().left(64);
		if (const auto color = proto::validColor(message.value("color"))) { client.color = *color; }
		qInfo("[%s] hello from %s", qPrintable(client.clientId), qPrintable(client.user));
		send(client, {{"t", proto::msg::Welcome}, {"proto", proto::Version}, {"clientId", client.clientId}});
	}
	else if (client.user.isEmpty())
	{
		sendError(client, "expected hello");
	}
	else if (t == proto::msg::Create) { handleCreate(client, message); }
	else if (t == proto::msg::Open) { handleOpen(client, message); }
	else if (t == proto::msg::Tx) { handleTx(client, message); }
	else if (t == proto::msg::Presence) { handlePresence(client, message); }
	else { sendError(client, "unknown message type " + t.left(32)); }
}


void CollabServer::handleCreate(Client& client, const QJsonObject& message)
{
	const QString name = message.value("project").toString();
	if (!isValidProjectName(name)) { return sendError(client, "invalid project name"); }
	if (findOrLoadProject(name)) { return sendError(client, "project \"" + name + "\" already exists"); }

	auto project = std::make_unique<Project>();
	project->name = name;
	QString error;
	if (!project->state.load(message.value("mmp").toString().toUtf8(), error))
	{
		return sendError(client, "cannot create project: " + error);
	}
	project->dirty = true;
	Project* p = project.get();
	m_projects[name] = std::move(project);
	saveProject(*p);

	client.project = p;
	qInfo("[%s] %s created project \"%s\" (%d pattern clips)", qPrintable(client.clientId), qPrintable(client.user),
		qPrintable(name), p->state.clipCount());
	send(client, {{"t", proto::msg::Joined}, {"project", name}, {"seq", p->seq}});
	sendPresenceOfOthers(client);
}


void CollabServer::handleOpen(Client& client, const QJsonObject& message)
{
	const QString name = message.value("project").toString();
	Project* p = isValidProjectName(name) ? findOrLoadProject(name) : nullptr;
	if (!p) { return sendError(client, "project \"" + name.left(64) + "\" does not exist"); }

	client.project = p;
	qInfo("[%s] %s opened project \"%s\" at seq %lld", qPrintable(client.clientId), qPrintable(client.user),
		qPrintable(name), p->seq);
	send(client, {{"t", proto::msg::Joined}, {"project", name}, {"seq", p->seq},
		{"mmp", QString::fromUtf8(p->state.toMmp())}});
	sendPresenceOfOthers(client);
}


void CollabServer::handlePresence(Client& client, const QJsonObject& message)
{
	if (!client.project) { return; }
	const auto presence = proto::sanitizePresence(message);
	if (!presence) { return; } // presence is best effort: ignore bad ones instead of disconnecting

	client.presence = *presence;
	client.presence.insert("t", proto::msg::Presence);
	client.presence.insert("clientId", client.clientId);
	client.presence.insert("user", client.user);
	client.presence.insert("color", client.color);

	// At most one forward per interval; a faster one is delayed, never dropped (the last one matters most)
	constexpr int MinIntervalMs = 20;
	const qint64 elapsed = client.presenceForwarded.isValid() ? client.presenceForwarded.elapsed() : MinIntervalMs;
	if (elapsed >= MinIntervalMs)
	{
		client.presenceForwarded.start();
		broadcast(client.project, client.presence, &client);
		return;
	}
	if (client.presencePending) { return; } // the scheduled forward will send the latest one
	client.presencePending = true;
	QTcpSocket* socket = client.socket;
	QTimer::singleShot(static_cast<int>(MinIntervalMs - elapsed), this, [this, socket] {
		const auto it = m_clients.find(socket);
		if (it == m_clients.end() || !it->second.project) { return; }
		it->second.presencePending = false;
		it->second.presenceForwarded.start();
		broadcast(it->second.project, it->second.presence, &it->second);
	});
}


void CollabServer::sendPresenceOfOthers(Client& client)
{
	for (auto& [socket, other] : m_clients)
	{
		if (&other != &client && other.project == client.project && !other.presence.isEmpty())
		{
			send(client, other.presence);
		}
	}
}


void CollabServer::handleTx(Client& client, const QJsonObject& message)
{
	Project* p = client.project;
	if (!p) { return sendError(client, "no project open"); }

	// Ops are applied in arrival order; the server order is the truth (last write wins).
	QJsonArray accepted;
	int rejected = 0;
	for (const QJsonValue& op : message.value("ops").toArray())
	{
		if (op.isObject() && p->state.apply(op.toObject())) { accepted.append(op); }
		else { ++rejected; }
	}
	if (!accepted.isEmpty())
	{
		p->dirty = true;
	}
	++p->seq;
	if (rejected > 0)
	{
		qInfo("[%s] tx %lld: %lld ops applied, %d rejected", qPrintable(client.clientId), p->seq,
			static_cast<long long>(accepted.size()), rejected);
	}
	// Everyone gets the transaction, including the sender (acknowledgement of its ctx)
	broadcast(p, {{"t", proto::msg::Tx}, {"seq", p->seq}, {"clientId", client.clientId},
		{"ctx", message.value("ctx")}, {"ops", accepted}});
}


void CollabServer::send(Client& client, const QJsonObject& message)
{
	client.socket->write(proto::encodeJsonFrame(message));
}


void CollabServer::sendError(Client& client, const QString& text)
{
	qWarning("[%s] error: %s", qPrintable(client.clientId), qPrintable(text));
	send(client, {{"t", proto::msg::Error}, {"message", text}});
}


void CollabServer::broadcast(Project* project, const QJsonObject& message, const Client* except)
{
	const QByteArray frame = proto::encodeJsonFrame(message);
	for (auto& [socket, client] : m_clients)
	{
		if (client.project == project && &client != except) { socket->write(frame); }
	}
}


QString CollabServer::projectDir(const QString& name) const
{
	return m_dataDir.filePath("projects/" + name);
}


CollabServer::Project* CollabServer::findOrLoadProject(const QString& name)
{
	if (const auto it = m_projects.find(name); it != m_projects.end()) { return it->second.get(); }

	QFile file{projectDir(name) + "/" + LiveFile};
	if (!file.open(QIODevice::ReadOnly)) { return nullptr; }

	auto project = std::make_unique<Project>();
	project->name = name;
	QString error;
	if (!project->state.load(file.readAll(), error))
	{
		qCritical("Cannot load project \"%s\": %s", qPrintable(name), qPrintable(error));
		return nullptr;
	}
	QFile manifest{projectDir(name) + "/" + ManifestFile};
	if (manifest.open(QIODevice::ReadOnly))
	{
		project->seq = QJsonDocument::fromJson(manifest.readAll()).object().value("seq").toInteger();
	}
	qInfo("Loaded project \"%s\" from disk (seq %lld)", qPrintable(name), project->seq);
	Project* p = project.get();
	m_projects[name] = std::move(project);
	return p;
}


bool CollabServer::saveProject(Project& project)
{
	const QString dir = projectDir(project.name);
	if (!QDir{}.mkpath(dir + "/live"))
	{
		qCritical("Cannot create %s", qPrintable(dir));
		return false;
	}
	// QSaveFile writes to a temporary file and renames it, so a crash never leaves a half-written project
	QSaveFile live{dir + "/" + LiveFile};
	QSaveFile manifest{dir + "/" + ManifestFile};
	const QJsonObject meta{{"name", project.name}, {"formatVersion", 1}, {"seq", project.seq},
		{"savedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
	const bool ok = live.open(QIODevice::WriteOnly) && live.write(project.state.toMmp()) >= 0 && live.commit()
		&& manifest.open(QIODevice::WriteOnly) && manifest.write(QJsonDocument{meta}.toJson()) >= 0
		&& manifest.commit();
	if (!ok)
	{
		qCritical("Cannot save project \"%s\"", qPrintable(project.name));
		return false;
	}
	project.dirty = false;
	return true;
}


void CollabServer::saveAll()
{
	for (auto& [name, project] : m_projects)
	{
		if (project->dirty && saveProject(*project))
		{
			qInfo("Saved project \"%s\" (seq %lld)", qPrintable(name), project->seq);
		}
	}
}

} // namespace lmms::collab
