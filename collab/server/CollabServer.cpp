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

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
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
const QString LibraryFile = "library.json";
const QString LibraryDir = "library";
constexpr int MaxUploadsPerClient = 8;
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
	dropUploads(it->second);
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
		if (type == proto::FrameType::Binary)
		{
			handleBinary(client, payload);
			if (m_clients.find(socket) == m_clients.end()) { return; }
			continue;
		}
		const auto message = proto::parseMessage(payload);
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
	else if (t == proto::msg::AssetPut) { handleAssetPut(client, message); }
	else if (t == proto::msg::AssetGet) { handleAssetGet(client, message); }
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
	send(client, {{"t", proto::msg::Joined}, {"project", name}, {"seq", p->seq}, {"library", libraryList(*p)}});
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
		{"mmp", QString::fromUtf8(p->state.toMmp())}, {"library", libraryList(*p)}});
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
		bool ok = false;
		if (op.toObject().value("op").toString() == proto::op::LibraryAdd)
		{
			// Announces a file this server stored (asset.stored): it must be there, under that path
			const QJsonObject o = op.toObject();
			const auto asset = p->library.constFind(o.value("hash").toString());
			ok = asset != p->library.cend() && asset->path == o.value("path").toString()
				&& asset->size == o.value("size").toInteger();
			if (ok)
			{
				// Who shared it, as the others see it, is who sent it
				QJsonObject announced = o;
				announced.insert("by", client.user);
				accepted.append(announced);
				continue;
			}
		}
		else { ok = op.isObject() && p->state.apply(op.toObject()); }
		if (ok) { accepted.append(op); }
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


QJsonArray CollabServer::libraryList(const Project& project)
{
	QJsonArray list;
	for (auto it = project.library.cbegin(); it != project.library.cend(); ++it)
	{
		list.append(QJsonObject{{"path", it->path}, {"hash", it.key()}, {"size", it->size}});
	}
	return list;
}


bool CollabServer::saveLibrary(const Project& project)
{
	QSaveFile file{projectDir(project.name) + "/" + LibraryFile};
	return file.open(QIODevice::WriteOnly)
		&& file.write(QJsonDocument{QJsonObject{{"files", libraryList(project)}}}.toJson()) >= 0 && file.commit();
}


void CollabServer::handleAssetPut(Client& client, const QJsonObject& message)
{
	Project* p = client.project;
	const QString hash = message.value("hash").toString();
	const qint64 size = message.value("size").toInteger();
	const QString name = proto::sanitizeAssetName(message.value("name").toString());
	auto fail = [&](const QString& text) {
		qWarning("[%s] upload refused: %s", qPrintable(client.clientId), qPrintable(text));
		send(client, {{"t", proto::msg::AssetError}, {"hash", hash.left(64)}, {"message", text}});
	};
	if (!p) { return fail("no project open"); }
	if (!proto::isValidHash(hash)) { return fail("invalid hash"); }
	if (size <= 0 || size > proto::MaxAssetSize) { return fail("file too large"); }
	if (name.isEmpty()) { return fail("this kind of file cannot be shared"); }

	// The same content is stored once
	if (const auto known = p->library.constFind(hash); known != p->library.cend())
	{
		return send(client, {{"t", proto::msg::AssetStored}, {"hash", hash}, {"path", known->path}});
	}
	if (client.uploads.count(hash)) { return; }
	if (static_cast<int>(client.uploads.size()) >= MaxUploadsPerClient) { return fail("too many uploads at once"); }

	const QString dir = projectDir(p->name) + "/" + LibraryDir;
	QDir{}.mkpath(dir);
	auto file = std::make_unique<QFile>(dir + "/.part-" + client.clientId + "-" + hash);
	if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) { return fail("cannot store the file"); }
	client.uploads[hash] = Upload{name, size, 0, std::move(file)};
	qInfo("[%s] uploading %s (%lld bytes)", qPrintable(client.clientId), qPrintable(name), static_cast<long long>(size));
}


void CollabServer::handleBinary(Client& client, const QByteArray& payload)
{
	if (payload.size() < proto::AssetHashSize)
	{
		qWarning("[%s] invalid binary frame, closing connection", qPrintable(client.clientId));
		client.socket->abort();
		return;
	}
	const QString hash = QString::fromLatin1(payload.left(proto::AssetHashSize).toHex());
	const auto it = client.uploads.find(hash);
	if (it == client.uploads.end() || !client.project) { return; } // e.g. an upload refused earlier
	Upload& upload = it->second;
	const QByteArray data = payload.mid(proto::AssetHashSize);
	auto fail = [&](const QString& text) {
		qWarning("[%s] upload of %s failed: %s", qPrintable(client.clientId), qPrintable(upload.name), qPrintable(text));
		upload.file->remove();
		send(client, {{"t", proto::msg::AssetError}, {"hash", hash}, {"message", text}});
		client.uploads.erase(it);
	};
	if (upload.received + data.size() > upload.size) { return fail("more data than announced"); }
	if (upload.file->write(data) != data.size()) { return fail("cannot store the file"); }
	upload.received += data.size();
	if (upload.received < upload.size) { return; }

	// Complete: the content must be what was announced
	upload.file->close();
	QCryptographicHash sha{QCryptographicHash::Sha256};
	if (!upload.file->open(QIODevice::ReadOnly) || !sha.addData(upload.file.get()))
	{
		return fail("cannot read the stored file");
	}
	upload.file->close();
	if (QString::fromLatin1(sha.result().toHex()) != hash) { return fail("the file arrived damaged"); }

	// A free name in the library: "name.wav", "name (2).wav", ...
	Project* p = client.project;
	const QString dir = projectDir(p->name) + "/" + LibraryDir;
	const QString base = upload.name.section('.', 0, -2);
	const QString suffix = upload.name.section('.', -1);
	QString path = upload.name;
	for (int n = 2; QFileInfo::exists(dir + "/" + path); ++n) { path = QString{"%1 (%2).%3"}.arg(base).arg(n).arg(suffix); }
	if (!upload.file->rename(dir + "/" + path)) { return fail("cannot store the file"); }

	p->library.insert(hash, Project::Asset{path, upload.size});
	saveLibrary(*p);
	qInfo("[%s] stored shared file %s (%lld bytes)", qPrintable(client.clientId), qPrintable(path),
		static_cast<long long>(upload.size));
	send(client, {{"t", proto::msg::AssetStored}, {"hash", hash}, {"path", path}});
	client.uploads.erase(it);
}


void CollabServer::handleAssetGet(Client& client, const QJsonObject& message)
{
	Project* p = client.project;
	const QString hash = message.value("hash").toString();
	const auto asset = p ? p->library.constFind(hash) : QHash<QString, Project::Asset>::const_iterator{};
	QFile file{p ? projectDir(p->name) + "/" + LibraryDir + "/" + (asset != p->library.cend() ? asset->path : QString{}) : QString{}};
	if (!p || asset == p->library.cend() || !file.open(QIODevice::ReadOnly))
	{
		return send(client, {{"t", proto::msg::AssetError}, {"hash", hash.left(64)}, {"message", "no such file"}});
	}
	send(client, {{"t", proto::msg::AssetData}, {"hash", hash}, {"size", asset->size}});
	const QByteArray rawHash = QByteArray::fromHex(hash.toLatin1());
	while (!file.atEnd()) { client.socket->write(proto::encodeBinaryFrame(rawHash, file.read(proto::AssetChunkSize))); }
}


void CollabServer::dropUploads(Client& client)
{
	for (auto& [hash, upload] : client.uploads) { upload.file->remove(); }
	client.uploads.clear();
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
	QFile library{projectDir(name) + "/" + LibraryFile};
	if (library.open(QIODevice::ReadOnly))
	{
		for (const QJsonValue& v : QJsonDocument::fromJson(library.readAll()).object().value("files").toArray())
		{
			const QJsonObject f = v.toObject();
			const QString hash = f.value("hash").toString();
			const QString path = proto::sanitizeAssetName(f.value("path").toString());
			// Only files that are really there
			if (proto::isValidHash(hash) && !path.isEmpty() && QFileInfo::exists(projectDir(name) + "/" + LibraryDir + "/" + path))
			{
				project->library.insert(hash, Project::Asset{path, f.value("size").toInteger()});
			}
		}
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
