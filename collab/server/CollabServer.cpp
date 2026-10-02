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
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>

#include <algorithm>

namespace lmms::collab
{

namespace
{
constexpr int SaveIntervalMs = 5000;
const QString LiveFile = "live/project.mmp";
const QString ManifestFile = "manifest.json";
const QString LibraryFile = "library.json";
const QString LibraryDir = "library";
const QString VersionsDir = "versions";
constexpr int MaxUploadsPerClient = 8;
// Wrong passwords: after this many from one address, it waits a while before trying again
constexpr int AuthFailuresAllowed = 5;
constexpr int AuthBlockSeconds = 60;
constexpr int AuthFailureMemorySeconds = 10 * 60;
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
	// A Perforce submit that is running is finished (queued ones are not started)
	if (m_p4Thread.joinable()) { m_p4Thread.join(); }
}


void CollabServer::setPerforce(const P4Exporter::Config& config)
{
	m_p4 = config;
}


bool CollabServer::setTls(const QString& certificateFile, const QString& keyFile)
{
	if (!QSslSocket::supportsSsl())
	{
		qCritical("Encryption (TLS) is not available here: %s", qPrintable(QSslSocket::activeBackend()));
		return false;
	}
	QFile certFile{certificateFile};
	QFile key{keyFile};
	if (!certFile.open(QIODevice::ReadOnly) || !key.open(QIODevice::ReadOnly))
	{
		qCritical("Cannot read the certificate %s or its key %s", qPrintable(certificateFile), qPrintable(keyFile));
		return false;
	}
	const QSslCertificate certificate{certFile.readAll(), QSsl::Pem};
	const QByteArray keyData = key.readAll();
	QSslKey privateKey{keyData, QSsl::Ec, QSsl::Pem};
	if (privateKey.isNull()) { privateKey = QSslKey{keyData, QSsl::Rsa, QSsl::Pem}; }
	if (certificate.isNull() || privateKey.isNull())
	{
		qCritical("The certificate %s or its key %s is not usable", qPrintable(certificateFile), qPrintable(keyFile));
		return false;
	}
	QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
	configuration.setLocalCertificate(certificate);
	configuration.setPrivateKey(privateKey);
	configuration.setProtocol(QSsl::TlsV1_2OrLater);
	configuration.setPeerVerifyMode(QSslSocket::VerifyNone); // clients have no certificates

	auto server = new QSslServer(this);
	server->setSslConfiguration(configuration);
	delete m_server;
	m_server = server;
	connect(m_server, &QTcpServer::pendingConnectionAvailable, this, &CollabServer::onNewConnection);
	connect(server, &QSslServer::errorOccurred, this, [](QSslSocket* socket, QAbstractSocket::SocketError) {
		qInfo("TLS connection from %s failed: %s", qPrintable(socket->peerAddress().toString()),
			qPrintable(socket->errorString()));
	});
	m_tls = true;
	qInfo("Connections are encrypted (TLS), certificate SHA-256 %s",
		certificate.digest(QCryptographicHash::Sha256).toHex(':').constData());
	return true;
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
		// Until it proves it may be here, nothing large is read from it
		client.decoder.setMaxFrameSize(proto::MaxUnauthenticatedFrameSize);
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

	const QByteArray data = socket->readAll();
	if (!client.receivedData && !m_tls && data.startsWith(QByteArrayView{"\x16\x03", 2}))
	{
		// A TLS handshake: LMMS tries an encrypted connection first, and connects again without encryption
		socket->abort();
		return;
	}
	client.receivedData = true;
	client.decoder.append(data);
	proto::FrameType type;
	QByteArray payload;
	while (client.decoder.next(type, payload))
	{
		if (type == proto::FrameType::Binary)
		{
			if (!client.authenticated) { socket->abort(); return; }
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
	if (t == proto::msg::Hello) { return handleHello(client, message); }
	if (t == proto::msg::Auth) { return handleAuth(client, message); }
	if (client.user.isEmpty() || !client.authenticated)
	{
		sendError(client, client.user.isEmpty() ? "expected hello" : "expected the password");
		client.socket->disconnectFromHost();
		return;
	}
	if (t == proto::msg::List) { handleList(client); }
	else if (t == proto::msg::Save)
	{
		if (client.project && (!client.project->dirty || saveProject(*client.project))) { announceSaved(*client.project); }
	}
	else if (t == proto::msg::Create) { handleCreate(client, message); }
	else if (t == proto::msg::Open) { handleOpen(client, message); }
	else if (t == proto::msg::Tx) { handleTx(client, message); }
	else if (t == proto::msg::Presence) { handlePresence(client, message); }
	else if (t == proto::msg::AssetPut) { handleAssetPut(client, message); }
	else if (t == proto::msg::AssetGet) { handleAssetGet(client, message); }
	else if (t == proto::msg::VersionCreate) { handleVersionCreate(client, message); }
	else if (t == proto::msg::VersionsGet) { handleVersionsGet(client); }
	else if (t == proto::msg::VersionRestore) { handleVersionRestore(client, message); }
	else { sendError(client, "unknown message type " + t.left(32)); }
}


void CollabServer::handleHello(Client& client, const QJsonObject& message)
{
	if (!client.user.isEmpty()) { return sendError(client, "hello was already sent"); }
	if (message.value("proto").toInt() != proto::Version)
	{
		sendError(client, QString{"protocol version mismatch (server %1): this LMMS and the server are different "
			"versions"}.arg(proto::Version));
		client.socket->disconnectFromHost();
		return;
	}
	client.user = message.value("user").toString().left(64);
	if (client.user.isEmpty()) { client.user = "?"; }
	if (const auto color = proto::validColor(message.value("color"))) { client.color = *color; }
	qInfo("[%s] hello from %s", qPrintable(client.clientId), qPrintable(client.user));
	if (m_passwordKey.isEmpty()) { return welcome(client); }

	const QString address = client.socket->peerAddress().toString();
	if (const int wait = authBlockedFor(address); wait > 0)
	{
		sendError(client, QString{"too many wrong passwords: try again in %1 seconds"}.arg(wait));
		client.socket->disconnectFromHost();
		return;
	}
	client.challenge.resize(32);
	QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(client.challenge.data()), 32 / sizeof(quint32));
	send(client, {{"t", proto::msg::Auth}, {"challenge", QString::fromLatin1(client.challenge.toHex())}});
}


void CollabServer::handleAuth(Client& client, const QJsonObject& message)
{
	if (client.challenge.isEmpty() || client.authenticated) { return sendError(client, "unexpected auth"); }
	const QByteArray response = QByteArray::fromHex(message.value("response").toString().toLatin1());
	const QByteArray expected = proto::authResponse(m_passwordKey, client.challenge);
	client.challenge.clear(); // one answer per challenge
	// Compared in constant time: how long it takes says nothing about how close a guess was
	bool same = response.size() == expected.size();
	unsigned char difference = 0;
	for (qsizetype i = 0; same && i < expected.size(); ++i) { difference |= response[i] ^ expected[i]; }
	same = same && difference == 0;

	const QString address = client.socket->peerAddress().toString();
	if (!same)
	{
		AuthFailures& failures = m_authFailures[address];
		if (failures.last.isValid() && failures.last.secsTo(QDateTime::currentDateTimeUtc()) > AuthFailureMemorySeconds)
		{
			failures.count = 0;
		}
		++failures.count;
		failures.last = QDateTime::currentDateTimeUtc();
		qWarning("[%s] wrong password from %s (%d)", qPrintable(client.clientId), qPrintable(address), failures.count);
		sendError(client, "wrong password");
		client.socket->disconnectFromHost();
		return;
	}
	m_authFailures.remove(address);
	welcome(client);
}


int CollabServer::authBlockedFor(const QString& address) const
{
	const auto it = m_authFailures.constFind(address);
	if (it == m_authFailures.cend() || it->count < AuthFailuresAllowed) { return 0; }
	const qint64 since = it->last.secsTo(QDateTime::currentDateTimeUtc());
	return since < AuthBlockSeconds ? static_cast<int>(AuthBlockSeconds - since) : 0;
}


void CollabServer::welcome(Client& client)
{
	client.authenticated = true;
	client.decoder.setMaxFrameSize(proto::MaxFrameSize);
	auto ssl = qobject_cast<QSslSocket*>(client.socket);
	send(client, {{"t", proto::msg::Welcome}, {"proto", proto::Version}, {"clientId", client.clientId},
		{"encrypted", ssl && ssl->isEncrypted()}});
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
	send(client, {{"t", proto::msg::Joined}, {"project", name}, {"seq", p->seq}, {"library", libraryList(*p)},
		{"savedSeq", p->savedSeq}});
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
		{"mmp", QString::fromUtf8(p->state.toMmp())}, {"library", libraryList(*p)}, {"savedSeq", p->savedSeq}});
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
		else
		{
			++rejected;
			// What exactly, to understand it later (without the large XML)
			QJsonObject summary = op.toObject();
			summary.remove("xml");
			qInfo("[%s] rejected %s", qPrintable(client.clientId),
				QJsonDocument{summary}.toJson(QJsonDocument::Compact).left(300).constData());
		}
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
	// The sender learns when some of its ops were not applied: its model differs from the project then
	QJsonObject tx{{"t", proto::msg::Tx}, {"seq", p->seq}, {"clientId", client.clientId},
		{"ctx", message.value("ctx")}, {"ops", accepted}};
	if (rejected > 0) { tx.insert("rejected", rejected); }
	broadcast(p, tx);
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
	project->savedSeq = project->seq;
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
	project.savedSeq = project.seq;
	return true;
}


void CollabServer::announceSaved(Project& project)
{
	broadcast(&project, {{"t", proto::msg::Saved}, {"seq", project.savedSeq},
		{"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
}


void CollabServer::handleList(Client& client)
{
	// Projects on disk, and new ones not saved yet
	QStringList names = QDir{m_dataDir.filePath("projects")}.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto& [name, project] : m_projects)
	{
		if (!names.contains(name)) { names.append(name); }
	}
	QJsonArray projects;
	for (const QString& name : names)
	{
		if (!isValidProjectName(name)) { continue; }
		const QFileInfo live{projectDir(name) + "/" + LiveFile};
		if (!live.exists() && !m_projects.count(name)) { continue; }
		int users = 0;
		for (const auto& [socket, other] : m_clients)
		{
			if (other.project && other.project->name == name) { ++users; }
		}
		projects.append(QJsonObject{{"name", name},
			{"modified", live.exists() ? live.lastModified().toUTC().toString(Qt::ISODate) : QString{}}, {"users", users}});
	}
	send(client, {{"t", proto::msg::Projects}, {"projects", projects}});
}


// ------------------------------------------------------------------------------------------------
// Versions: <project>/versions/<id>/ {project.mmp, library.json, version.json}. Shared files are never removed
// from the library, so a version only lists them.

void CollabServer::handleVersionCreate(Client& client, const QJsonObject& message)
{
	auto refuse = [&](const QString& text) { send(client, {{"t", proto::msg::VersionError}, {"message", text}}); };
	Project* p = client.project;
	if (!p) { return refuse("not in a project"); }
	const QString description = message.value("description").toString().trimmed();
	if (description.isEmpty()) { return refuse("a version needs a description"); }
	if (description.size() > proto::MaxVersionDescription) { return refuse("the description is too long"); }
	QString error;
	if (!createVersion(*p, client, description, error)) { refuse(error); }
}


void CollabServer::handleVersionRestore(Client& client, const QJsonObject& message)
{
	auto refuse = [&](const QString& text) { send(client, {{"t", proto::msg::VersionError}, {"message", text}}); };
	Project* p = client.project;
	if (!p) { return refuse("not in a project"); }
	const int id = message.value("id").toInt();
	QJsonObject restored;
	for (const QJsonValue& v : versionList(p->name))
	{
		if (v.toObject().value("id").toInt() == id) { restored = v.toObject(); }
	}
	if (id <= 0 || restored.isEmpty()) { return refuse(QString{"there is no version %1"}.arg(id)); }
	QFile file{projectDir(p->name) + "/" + VersionsDir + "/" + QString::number(id) + "/project.mmp"};
	QString error;
	ProjectState check;
	const QByteArray mmp = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
	if (!check.load(mmp, error)) { return refuse(QString{"version %1 cannot be read: %2"}.arg(id).arg(error)); }

	// Nothing is lost: the project as it is now becomes a version first
	const auto safety = createVersion(*p, client, QString{"Before restoring version %1"}.arg(id), error);
	if (!safety) { return refuse(error); }
	if (!p->state.load(mmp, error)) { return refuse(error); } // cannot fail: it was read above
	++p->seq;
	p->dirty = true;
	if (saveProject(*p)) { announceSaved(*p); }
	qInfo("[%s] %s restored version %d of \"%s\" (the state before is version %d)", qPrintable(client.clientId),
		qPrintable(client.user), id, qPrintable(p->name), safety->value("id").toInt());
	// Everybody loads the project again (shared files are never removed, so the library has all it uses)
	broadcast(p, {{"t", proto::msg::VersionRestored}, {"id", id}, {"by", client.user},
		{"description", restored.value("description")}, {"safety", safety->value("id")}, {"seq", p->seq},
		{"mmp", QString::fromUtf8(p->state.toMmp())}, {"library", libraryList(*p)}, {"savedSeq", p->savedSeq}});
}


std::optional<QJsonObject> CollabServer::createVersion(Project& target, const Client& client,
	const QString& description, QString& error)
{
	Project* p = &target;
	// The version is the project as it is now: every transaction before this request is in it
	if (p->dirty)
	{
		if (!saveProject(*p))
		{
			error = "the project could not be saved on the server";
			return std::nullopt;
		}
		announceSaved(*p);
	}
	const QJsonArray versions = versionList(p->name);
	const int id = versions.isEmpty() ? 1 : versions.last().toObject().value("id").toInt() + 1;
	const QString dir = projectDir(p->name) + "/" + VersionsDir + "/" + QString::number(id);
	const QByteArray mmp = p->state.toMmp();
	QSaveFile project{dir + "/project.mmp"};
	QSaveFile library{dir + "/" + LibraryFile};
	const bool ok = QDir{}.mkpath(dir)
		&& project.open(QIODevice::WriteOnly) && project.write(mmp) >= 0 && project.commit()
		&& library.open(QIODevice::WriteOnly)
		&& library.write(QJsonDocument{QJsonObject{{"files", libraryList(*p)}}}.toJson()) >= 0 && library.commit();
	const QJsonObject version{{"id", id}, {"description", description}, {"by", client.user},
		{"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"seq", p->seq},
		{"p4", QJsonObject{{"state", m_p4 ? "pending" : "off"}}}};
	if (!ok || !writeVersion(p->name, version))
	{
		QDir{dir}.removeRecursively();
		error = "the version could not be written on the server";
		return std::nullopt;
	}
	qInfo("[%s] %s created version %d of \"%s\": %s", qPrintable(client.clientId), qPrintable(client.user), id,
		qPrintable(p->name), qPrintable(description.section('\n', 0, 0).left(80)));
	broadcast(p, {{"t", proto::msg::VersionCreated}, {"version", version}});

	if (m_p4)
	{
		P4Job job{id, {p->name, mmp, {}, description + "\n\n(" + client.user + ", LMMS-Collab version " + QString::number(id) + ")"}};
		for (const Project::Asset& asset : p->library)
		{
			job.job.files.append({asset.path, projectDir(p->name) + "/" + LibraryDir + "/" + asset.path});
		}
		m_p4Queue.push_back(std::move(job));
		startP4Job();
	}
	return version;
}


void CollabServer::handleVersionsGet(Client& client)
{
	if (!client.project)
	{
		return send(client, {{"t", proto::msg::VersionError}, {"message", "not in a project"}});
	}
	send(client, {{"t", proto::msg::Versions}, {"versions", versionList(client.project->name)}});
}


QJsonArray CollabServer::versionList(const QString& project) const
{
	const QDir dir{projectDir(project) + "/" + VersionsDir};
	std::vector<std::pair<int, QJsonObject>> versions;
	for (const QString& entry : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
	{
		bool number = false;
		const int id = entry.toInt(&number);
		QFile file{dir.filePath(entry + "/version.json")};
		if (!number || id <= 0 || !file.open(QIODevice::ReadOnly)) { continue; }
		const QJsonObject version = QJsonDocument::fromJson(file.readAll()).object();
		if (version.value("id").toInt() == id) { versions.emplace_back(id, version); }
	}
	std::sort(versions.begin(), versions.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	QJsonArray list;
	for (const auto& [id, version] : versions) { list.append(version); }
	return list;
}


bool CollabServer::writeVersion(const QString& project, const QJsonObject& version)
{
	QSaveFile file{projectDir(project) + "/" + VersionsDir + "/" + QString::number(version.value("id").toInt())
		+ "/version.json"};
	return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument{version}.toJson()) >= 0 && file.commit();
}


void CollabServer::startP4Job()
{
	if (m_p4Busy || m_p4Queue.empty() || !m_p4) { return; }
	if (m_p4Thread.joinable()) { m_p4Thread.join(); } // the previous one has ended
	P4Job job = std::move(m_p4Queue.front());
	m_p4Queue.pop_front();
	m_p4Busy = true;
	qInfo("Submitting version %d of \"%s\" to Perforce", job.id, qPrintable(job.job.project));
	// Perforce may take a while (large files): the sessions go on meanwhile
	m_p4Thread = std::thread{[this, config = *m_p4, job = std::move(job)] {
		const P4Exporter::Result result = P4Exporter::submit(config, job.job);
		QMetaObject::invokeMethod(this, [this, project = job.job.project, id = job.id, result] {
			p4JobDone(project, id, result);
		}, Qt::QueuedConnection);
	}};
}


void CollabServer::p4JobDone(const QString& project, int id, const P4Exporter::Result& result)
{
	m_p4Busy = false;
	QJsonObject p4;
	switch (result.status)
	{
	case P4Exporter::Result::Status::Submitted:
		p4 = {{"state", "submitted"}, {"change", result.change}};
		qInfo("Version %d of \"%s\" submitted to Perforce as change %d", id, qPrintable(project), result.change);
		break;
	case P4Exporter::Result::Status::Unchanged:
		p4 = {{"state", "unchanged"}};
		qInfo("Version %d of \"%s\": nothing changed for Perforce", id, qPrintable(project));
		break;
	case P4Exporter::Result::Status::Failed:
		p4 = {{"state", "failed"}, {"error", result.error.left(1000)}};
		qWarning("Version %d of \"%s\" not submitted to Perforce: %s", id, qPrintable(project), qPrintable(result.error));
		break;
	}
	for (const QJsonValue& v : versionList(project))
	{
		QJsonObject version = v.toObject();
		if (version.value("id").toInt() != id) { continue; }
		version.insert("p4", p4);
		writeVersion(project, version);
	}
	if (const auto it = m_projects.find(project); it != m_projects.end())
	{
		broadcast(it->second.get(), {{"t", proto::msg::VersionP4}, {"id", id}, {"p4", p4}});
	}
	startP4Job();
}


void CollabServer::saveAll()
{
	for (auto& [name, project] : m_projects)
	{
		if (project->dirty && saveProject(*project))
		{
			qInfo("Saved project \"%s\" (seq %lld)", qPrintable(name), project->seq);
			announceSaved(*project);
		}
	}
}

} // namespace lmms::collab
