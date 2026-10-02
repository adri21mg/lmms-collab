/*
 * CollabSession.cpp - client side of a collaboration session
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

#include "CollabSession.h"
#include "Plugin.h"
#include "CollabLocalGesture.h"
#include "CollabSessionUtil.h"

#include <algorithm>

#include <QCoreApplication>
#include <utility>
#include <QDir>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QPushButton>
#include <QMessageBox>
#include <QFileDialog>
#include <QDomDocument>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QProgressDialog>
#include <QPointer>
#include <QScopeGuard>
#include <QTime>
#include <QTcpSocket>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "Mixer.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternEditor.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "ProjectNotes.h"
#include "Song.h"
#include "SongEditor.h"
#include "TrackContainerView.h"
#include "TrackView.h"

namespace lmms::collab
{

namespace
{
constexpr int FlushIntervalMs = 50;      // local changes are sent at most ~20 times per second
constexpr int StructureIntervalMs = 50;  // how often the Song Editor structure is compared with the baseline
constexpr int QueueRetryIntervalMs = 30; // how often to check whether a mouse gesture has ended

using proto::NoteValues;

//! Window geometry LMMS stores inside tracks (e.g. the instrument window); private (decision D9)
const QStringList WindowAttributes = {"x", "y", "width", "height", "visible", "maximized", "minimized", "tab"};

bool anySolo(const Track* track)
{
	const auto& tracks = track->trackContainer()->tracks();
	return std::any_of(tracks.begin(), tracks.end(), [](const Track* t) { return t->isSolo(); });
}

//! Solo is private and works by muting other tracks, so the shared mute is the one from before the solo
bool sharedMute(const Track* track)
{
	return anySolo(track) ? track->isMutedBeforeSolo() : track->isMuted();
}

QJsonObject withoutPrivateKeys(QJsonObject fields)
{
	for (const QString& key : fields.keys())
	{
		if (key.startsWith('_')) { fields.remove(key); }
	}
	return fields;
}

} // namespace


NoteState NoteState::of(const Note& note)
{
	NoteState s;
	s.fields[NoteValues::Key] = note.key();
	s.fields[NoteValues::Pos] = note.pos().getTicks();
	s.fields[NoteValues::Len] = note.length().getTicks();
	s.fields[NoteValues::Vol] = note.getVolume();
	s.fields[NoteValues::Pan] = note.getPanning();
	s.fields[NoteValues::Type] = static_cast<int>(note.type());
	return s;
}


proto::NoteValues NoteState::toValues() const
{
	NoteValues v;
	for (int f = 0; f < NoteValues::FieldCount; ++f) { v.fields[f] = fields[f]; }
	return v;
}


std::optional<NoteState> NoteState::fromJson(const QJsonObject& obj)
{
	const auto values = NoteValues::fromJson(obj);
	if (!values || !values->isComplete()) { return std::nullopt; }
	NoteState s;
	for (int f = 0; f < NoteValues::FieldCount; ++f) { s.fields[f] = *values->fields[f]; }
	return s;
}


//! Throttles change detection of one shared clip
struct CollabSession::ClipTracker
{
	QTimer timer;
	QMetaObject::Connection changed;
	QMetaObject::Connection destroyed;

	~ClipTracker()
	{
		QObject::disconnect(changed);
		QObject::disconnect(destroyed);
	}
};


CollabSession* CollabSession::instance()
{
	static QPointer<CollabSession> s_instance;
	if (!s_instance)
	{
		s_instance = new CollabSession;
		s_instance->setParent(QCoreApplication::instance());
	}
	return s_instance;
}


CollabSession::CollabSession() :
	m_txQueueTimer(new QTimer(this)),
	m_structureTimer(new QTimer(this)),
	m_trackStateTimer(new QTimer(this))
{
	m_txQueueTimer->setSingleShot(true);
	connect(m_txQueueTimer, &QTimer::timeout, this, &CollabSession::processTxQueue);
	connect(m_structureTimer, &QTimer::timeout, this, [this] {
		flushStructure();
		flushPlugins();
		flushParams();
	});
	connect(m_trackStateTimer, &QTimer::timeout, this, &CollabSession::sendTrackStates);

	// Plugins that are not installed here: in a session one notice lists them all (instead of a message box
	// per plugin, possibly in the middle of applying someone else's change); their settings are kept
	Plugin::setMissingPluginHandler([this](const QString& name, const QString& reason) {
		if (m_state == State::Disconnected && !m_reconnecting) { return false; }
		notePluginMissing(name, reason);
		return true;
	});
}


void CollabSession::notePluginMissing(const QString& name, const QString& reason)
{
	if (m_missingPlugins.contains(name)) { return; }
	log(QString{"plugin %1 is not available here: %2"}.arg(name, reason));
	m_missingPlugins.insert(name);
	m_newMissingPlugins.append(name);
	if (m_newMissingPlugins.size() == 1)
	{
		// Together with the others that come in the same moment (e.g. loading the project)
		QTimer::singleShot(300, this, [this] { emit missingPlugins(std::exchange(m_newMissingPlugins, {})); });
	}
}


CollabSession::~CollabSession()
{
	stopTracking();
}


// ------------------------------------------------------------------------------------------------
// Connection

void CollabSession::connectToServer(const QString& host, quint16 port, const QString& user,
	const QString& project, JoinMode mode, const QString& color, const QByteArray& passwordKey)
{
	disconnectFromServer();
	m_user = user;
	m_color = color;
	const QString logDir = ConfigManager::inst()->workingDir() + "collab/";
	QDir{}.mkpath(logDir);
	// The previous session's log is kept: it matters most after a crash, and LMMS is then started again.
	// Attempts to get a lost session back continue its log.
	if (!m_reconnecting || !m_log)
	{
		m_log.reset(); // closed, or Windows does not rename it
		QFile::remove(logDir + "session.previous.log");
		QFile::rename(logDir + "session.log", logDir + "session.previous.log");
		m_log = std::make_unique<QFile>(logDir + "session.log");
		if (!m_log->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) { m_log.reset(); }
	}
	log(QString{"connect %1:%2 as %3, project %4"}.arg(host).arg(port).arg(user, project));
	if (!m_reconnecting) { m_missingPlugins.clear(); } // told again in another session
	m_project = project;
	m_joinMode = mode;
	m_host = host;
	m_port = port;
	m_seq = 0;
	m_nextCtx = 1;
	m_ackedCtx = 0;
	m_unacknowledgedOps.clear();
	m_savedSeq = 0;
	m_savedAt = QDateTime{};

	m_passwordKey = passwordKey;
	m_encrypted = false;
	m_fingerprint.clear();

	// Encryption, the server's certificate and the password first (see CollabConnection)
	m_connection = std::make_unique<CollabConnection>();
	connect(m_connection.get(), &CollabConnection::ready, this, &CollabSession::onConnected);
	connect(m_connection.get(), &CollabConnection::failed, this,
		[this](CollabConnection::Failure failure, const QString& message) {
			const QString fingerprint = m_connection ? m_connection->fingerprint() : QString{};
			if (m_connection) { m_connection.release()->deleteLater(); } // this runs inside its own signal
			// A session that was going on (or is being got back) tries again while the server cannot be reached
			if (failure == CollabConnection::Failure::Network && (m_state == State::Live || m_reconnecting))
			{
				return startReconnecting();
			}
			log("connection failed: " + message);
			if (failure == CollabConnection::Failure::CertificateChanged)
			{
				emit serverCertificateChanged(m_host, m_port, fingerprint);
			}
			fail(failure == CollabConnection::Failure::Network ? tr("Connection error: %1").arg(message) : message);
		});
	setState(State::Connecting);
	m_connection->open(host, port, {{"t", proto::msg::Hello}, {"proto", proto::Version}, {"user", m_user},
		{"color", m_color}}, passwordKey);
}


void CollabSession::onConnected(const QJsonObject& welcome)
{
	QByteArray rest;
	m_socket = m_connection->takeSocket(rest);
	m_encrypted = m_connection->isEncrypted();
	m_fingerprint = m_connection->fingerprint();
	m_connection.release()->deleteLater(); // this runs inside its own signal
	log(m_encrypted ? "connection encrypted, server certificate " + m_fingerprint : QString{"connection not encrypted"});

	connect(m_socket.get(), &QTcpSocket::readyRead, this, &CollabSession::onReadyRead);
	connect(m_socket.get(), &QTcpSocket::disconnected, this, [this] {
		if (m_state == State::Live) { startReconnecting(); }
	});
	connect(m_socket.get(), &QTcpSocket::errorOccurred, this, [this] {
		// A session that was going on (or is being got back) tries again; a first connection reports the error
		if (m_state == State::Live || m_reconnecting) { return startReconnecting(); }
		fail(tr("Connection error: %1").arg(m_socket ? m_socket->errorString() : QString{}));
	});
	handleMessage(welcome);
	if (!rest.isEmpty() && m_socket)
	{
		m_decoder.append(rest);
		onReadyRead();
	}
}


void CollabSession::forgetProjectState()
{
	stopTracking();
	m_baselines.clear();
	m_structure = Structure{};
	m_pending.clear();
	m_pendingStructure.clear();
	m_gestures.clear();
	m_openGesture.clear();
	m_paramIndex.clear();
	m_paramBaseline.clear();
	m_pendingParams.clear();
	m_trackParams.clear();
	m_ctxParamTracks.clear();
	m_openParamGesture.clear();
	m_plugins.clear();
	m_editedTracks.clear();
	m_opaqueCheck.invalidate();
	m_txQueue.clear();
	m_txQueueTimer->stop();
	m_unresolvedAutomation.clear();
	m_linkBaseline.clear();
	m_pendingLinks.clear();
	m_unresolvedLinks.clear();
	m_forcePluginCheck.clear();
}


void CollabSession::disconnectFromServer()
{
	forgetProjectState();
	m_decoder = proto::FrameDecoder{};
	// Shared files on their way
	m_downloadQueue.clear();
	m_downloading.clear();
	m_downloadFile.reset();
	m_pendingJoin.reset();
	m_uploads.clear();
	m_shareQueue.clear();
	if (m_downloadProgress) { m_downloadProgress->close(); }
	emit presenceCleared();
	refreshSharedFiles(); // back to LMMS' own files
	if (m_connection)
	{
		m_connection->disconnect(this);
		m_connection.release()->deleteLater(); // may run inside one of its signals
	}
	if (m_socket)
	{
		// This may run inside one of the socket's own signal handlers, so delete it later
		QTcpSocket* socket = m_socket.release();
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
	}
	setState(State::Disconnected);
}


void CollabSession::leave()
{
	m_reconnecting = false;
	m_rejectedChanges = false;
	disconnectFromServer();
}


bool CollabSession::waitUntilSaved(int timeoutMs)
{
	if (m_state != State::Live) { return false; }
	saveNow();
	QElapsedTimer timer;
	timer.start();
	// The answer only counts once everything we sent was acknowledged and saved
	while (syncStatus() != SyncStatus::Saved && timer.elapsed() < timeoutMs && m_state == State::Live)
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	}
	return syncStatus() == SyncStatus::Saved && m_state == State::Live;
}


void CollabSession::startReconnecting()
{
	if (!m_reconnecting)
	{
		m_reconnecting = true;
		m_reconnectAttempts = 0;
		// What changes from now on was done offline. (LMMS does not mark every change as "modified", e.g. moving
		// a clip, so the project itself is compared when we are back.)
		m_unsentAtLoss = syncStatus() == SyncStatus::Sending;
		m_offlineSnapshot = projectSnapshot();
		log(QString{"connection lost: reconnecting%1"}.arg(m_unsentAtLoss ? " (own changes not sent yet)" : ""));
		Engine::getSong()->clearModified();
	}
	++m_reconnectAttempts;
	// Everything of the lost session goes (it is loaded again from the server); the intent to come back stays
	const QString host = m_host, user = m_user, project = m_project, color = m_color;
	const quint16 port = m_port;
	disconnectFromServer();
	setState(State::Reconnecting);
	QTimer::singleShot(m_reconnectAttempts == 1 ? 1000 : 3000, this, [=, this] {
		if (m_reconnecting && m_state == State::Reconnecting)
		{
			connectToServer(host, port, user, project, JoinMode::Open, color, m_passwordKey);
		}
	});
}


QString CollabSession::projectSnapshot()
{
	QTemporaryDir dir;
	const QString file = dir.filePath("snapshot.mmp");
	QFile f{file};
	QDomDocument doc;
	if (!dir.isValid() || !Engine::getSong()->saveProjectFile(file) || !f.open(QIODevice::ReadOnly)
		|| !doc.setContent(&f))
	{
		return {};
	}
	// Only what is shared: window geometry and the like are private (decision D9)
	const auto stripWindows = [](auto& self, QDomElement e) -> void {
		for (const char* attribute : {"x", "y", "width", "height", "visible", "maximized"})
		{
			e.removeAttribute(attribute);
		}
		for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) { self(self, c); }
	};
	const QDomElement song = doc.documentElement().firstChildElement("song");
	QStringList parts;
	{
		QString head; // tempo, master volume...
		QTextStream stream{&head};
		doc.documentElement().firstChildElement("head").save(stream, 0);
		parts.append(head);
	}
	for (QDomElement e = song.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
	{
		const QString tag = e.tagName();
		if (tag == "timeline" || tag == "pianoroll" || tag == "automationeditor") { continue; }
		stripWindows(stripWindows, e);
		QString text;
		QTextStream stream{&text};
		e.save(stream, 0);
		parts.append(text);
	}
	return parts.join('\n');
}


bool CollabSession::keepOfflineChanges()
{
	m_reconnecting = false;
	const bool changed = m_unsentAtLoss || Engine::getSong()->isModified() || projectSnapshot() != m_offlineSnapshot;
	const bool rejected = std::exchange(m_rejectedChanges, false);
	m_offlineSnapshot.clear();
	m_hadOfflineChanges = changed;
	log(QString{"offline changes: %1"}.arg(changed ? "yes" : "no"));
	if (!changed) { return true; }
	auto gui = gui::getGUI();
	if (!gui) { return true; }
	QMessageBox box{QMessageBox::Question, rejected ? tr("Project reloaded") : tr("Back online"),
		rejected ? tr("Some of your latest changes could not be added to the shared project, so the project was "
				"loaded again from the server (it continues as it is there). Your version, with those changes, can be "
				"kept as a file of its own (to copy parts of it back, for example).")
			: tr("You changed the project while the connection was lost. The project continues as it is on the server; "
				"your version can be kept as a file of its own (to copy parts of it back, for example)."),
		QMessageBox::NoButton, gui->mainWindow()};
	QPushButton* save = box.addButton(tr("Save my version as a file..."), QMessageBox::AcceptRole);
	box.addButton(tr("Discard my changes"), QMessageBox::DestructiveRole);
	box.exec();
	if (box.clickedButton() != save) { return true; }
	const QString file = QFileDialog::getSaveFileName(gui->mainWindow(), tr("Save my version"),
		ConfigManager::inst()->userProjectsDir() + m_project + " (offline).mmpz", tr("LMMS projects (*.mmpz *.mmp)"));
	if (!file.isEmpty() && !Engine::getSong()->saveProjectFile(file))
	{
		QMessageBox::warning(gui->mainWindow(), tr("Back online"), tr("Could not save %1").arg(file));
	}
	return true;
}


void CollabSession::setState(State state)
{
	if (m_state == state) { return; }
	m_state = state;
	emit stateChanged();
}


void CollabSession::fail(const QString& message)
{
	const bool wasActive = m_state != State::Disconnected;
	m_reconnecting = false; // e.g. the server answered, but the project is gone: trying again would not help
	m_rejectedChanges = false;
	disconnectFromServer();
	if (wasActive) { emit errorOccurred(message); }
}


void CollabSession::send(const QJsonObject& message)
{
	if (m_socket) { m_socket->write(proto::encodeJsonFrame(message)); }
}


void CollabSession::removeClipsNotIn(const QString& mmp)
{
	// The project just loaded is the shared one: a clip it does not have was made by LMMS meanwhile, on this
	// computer only (it would never be synchronized, and edits in it would be lost)
	static const QRegularExpression cidRe{"cid=\"([0-9a-f]{16})\""};
	QSet<collab_id_t> shared;
	for (auto it = cidRe.globalMatch(mmp); it.hasNext();) { shared.insert(proto::parseId(it.next().captured(1))); }
	std::vector<Track*> tracks = Engine::getSong()->tracks();
	for (Track* track : Engine::patternStore()->tracks()) { tracks.push_back(track); }
	for (Track* track : tracks)
	{
		const auto clips = track->getClips(); // a copy: clips are removed below
		for (Clip* clip : clips)
		{
			// Only empty MIDI clips (what an empty Piano Roll makes): nothing with content is ever removed here
			auto midiClip = dynamic_cast<MidiClip*>(clip);
			if (shared.contains(clip->collabId()) || !midiClip || !midiClip->notes().empty()) { continue; }
			log(QString{"removed empty clip %1 of track %2: not in the shared project (made while loading)"}
				.arg(proto::idString(clip->collabId()), proto::idString(track->collabId())));
			auto guard = Engine::audioEngine()->requestChangesGuard();
			delete clip;
		}
	}
}


QString CollabSession::opsSummary(const QJsonArray& ops)
{
	static const QRegularExpression cidRe{"cid=\"([0-9a-f]{16})\""};
	QStringList summary;
	for (const QJsonValue& op : ops)
	{
		QJsonObject o = op.toObject();
		// The XML is large and not needed to follow what happens; the id of what it holds is
		if (const auto match = cidRe.match(o.value("xml").toString()); match.hasMatch()) { o.insert("cid", match.captured(1)); }
		o.remove("xml");
		summary.append(QString::fromUtf8(QJsonDocument{o}.toJson(QJsonDocument::Compact)));
	}
	return summary.join(" ");
}


void CollabSession::resyncAfterRejection()
{
	if (m_state != State::Live || m_reconnecting) { return; }
	// Like a lost connection: the project is loaded again from the server, and this user's version can be kept
	m_rejectedChanges = true;
	startReconnecting();
	m_unsentAtLoss = true;
}


void CollabSession::sendOps(const QJsonArray& ops)
{
	if (m_log)
	{
		log(QString{"send ctx %1: %2"}.arg(m_nextCtx).arg(opsSummary(ops)));
	}
	m_unacknowledgedOps.insert(m_nextCtx, ops); // to tell what the server did not take
	send({{"t", proto::msg::Tx}, {"ctx", m_nextCtx++}, {"ops", ops}});
	emit syncStatusChanged();
}


CollabSession::SyncStatus CollabSession::syncStatus() const
{
	if (m_ackedCtx < m_nextCtx - 1) { return SyncStatus::Sending; }
	return m_savedSeq >= m_seq ? SyncStatus::Saved : SyncStatus::Saving;
}


void CollabSession::saveNow()
{
	if (m_state == State::Live) { send({{"t", proto::msg::Save}}); }
}


void CollabSession::createVersion(const QString& description)
{
	if (m_state != State::Live) { return; }
	flushAll(); // the latest local changes go first, so they are in the version
	send({{"t", proto::msg::VersionCreate}, {"description", description.left(proto::MaxVersionDescription)}});
}


void CollabSession::requestVersions()
{
	if (m_state == State::Live) { send({{"t", proto::msg::VersionsGet}}); }
}


void CollabSession::setColor(const QString& color)
{
	m_color = color;
	if (m_state == State::Live) { send({{"t", proto::msg::Color}, {"color", color}}); }
}


void CollabSession::restoreVersion(int id)
{
	if (m_state != State::Live) { return; }
	flushAll(); // so the safety version has everything
	send({{"t", proto::msg::VersionRestore}, {"id", id}});
}


void CollabSession::reloadRestored(const QJsonObject& message)
{
	log(QString{"version %1 restored by %2 (the state before is version %3): reloading"}
		.arg(message.value("id").toInt()).arg(message.value("by").toString()).arg(message.value("safety").toInt()));
	// Everything known about the project before the restore goes (what is still waiting was made on it, and its
	// baselines would make the restored state look like local changes, echoed to everyone)
	forgetProjectState();
	setState(State::Joining);
	emit versionRestored(message);
	// Like joining: the same project, as the server has it now
	handleJoined({{"project", m_project}, {"seq", message.value("seq")}, {"mmp", message.value("mmp")},
		{"library", message.value("library")}, {"savedSeq", message.value("savedSeq")}});
}


void CollabSession::log(const QString& line)
{
	if (!m_log) { return; }
	m_log->write(QString{"%1 %2\n"}.arg(QTime::currentTime().toString("hh:mm:ss.zzz"), line).toUtf8());
	m_log->flush();
}


void CollabSession::onReadyRead()
{
	// Dialogs and progress bars shown while handling a message process events, which would call this again
	// in the middle of a message: data that arrives meanwhile is read by the loop below instead
	if (m_reading) { return; }
	m_reading = true;
	const auto done = qScopeGuard([this] { m_reading = false; });
	proto::FrameType type;
	QByteArray payload;
	while (m_socket)
	{
		if (m_socket->bytesAvailable() > 0)
		{
			m_decoder.append(m_socket->readAll());
			continue;
		}
		if (!m_decoder.next(type, payload)) { break; }
		if (type == proto::FrameType::Binary)
		{
			handleBinary(payload); // a part of a shared file
			continue;
		}
		const auto message = proto::parseMessage(payload);
		if (!message) { return fail(tr("The server sent an invalid message.")); }
		handleMessage(*message);
	}
	if (m_decoder.hasError()) { fail(tr("The server sent an invalid message.")); }
}


void CollabSession::handleMessage(const QJsonObject& message)
{
	const QString t = message.value("t").toString();
	if (t == proto::msg::Welcome)
	{
		m_clientId = message.value("clientId").toString();
		setState(State::Joining);
		if (m_joinMode == JoinMode::Open)
		{
			send({{"t", proto::msg::Open}, {"project", m_project}});
			return;
		}
		// Share the current song as it is now; our model already has every id the server will use
		QTemporaryDir dir;
		const QString file = dir.filePath("share.mmp");
		QFile f{file};
		QDomDocument doc;
		if (!dir.isValid() || !Engine::getSong()->saveProjectFile(file) || !f.open(QIODevice::ReadOnly)
			|| !doc.setContent(&f))
		{
			return fail(tr("Could not serialize the current song."));
		}
		annotateAutomation(doc.documentElement()); // automated parameters by name, not by journal id
		send({{"t", proto::msg::Create}, {"project", m_project}, {"mmp", doc.toString(1)}});
	}
	else if (t == proto::msg::Joined) { handleJoined(message); }
	else if (t == proto::msg::Tx)
	{
		m_txQueue.append(message);
		processTxQueue();
	}
	else if (t == proto::msg::Presence)
	{
		// Presence never touches the project, so it is not queued behind mouse gestures
		if (m_state == State::Live) { emit presenceReceived(message); }
	}
	else if (t.startsWith("asset.")) { handleAssetMessage(message); }
	else if (t == proto::msg::VersionCreated)
	{
		const QJsonObject version = message.value("version").toObject();
		log(QString{"version %1 created by %2"}.arg(version.value("id").toInt()).arg(version.value("by").toString()));
		emit versionCreated(version);
	}
	else if (t == proto::msg::VersionP4)
	{
		emit versionPerforce(message.value("id").toInt(), message.value("p4").toObject());
	}
	else if (t == proto::msg::Versions) { emit versionsReceived(message.value("versions").toArray()); }
	else if (t == proto::msg::VersionError) { emit versionError(message.value("message").toString()); }
	else if (t == proto::msg::VersionRestored) { reloadRestored(message); }
	else if (t == proto::msg::Saved)
	{
		m_savedSeq = message.value("seq").toInteger();
		m_savedAt = QDateTime::currentDateTime();
		emit syncStatusChanged();
	}
	else if (t == proto::msg::Error) { fail(message.value("message").toString()); }
}


void CollabSession::sendPresence(const QJsonObject& presence)
{
	if (m_state != State::Live) { return; }
	QJsonObject message{{"t", proto::msg::Presence}};
	for (const char* part : {"cursor", "play", "view"})
	{
		const QJsonObject value = presence.value(part).toObject();
		message.insert(part, value.isEmpty() ? QJsonValue{QJsonValue::Null} : QJsonValue{value});
	}
	send(message);
}


void CollabSession::handleJoined(const QJsonObject& message)
{
	if (m_reconnecting)
	{
		log(QString{"reconnected after %1 attempt(s)"}.arg(m_reconnectAttempts));
		keepOfflineChanges();
	}
	m_seq = message.value("seq").toInteger();
	m_savedSeq = message.value("savedSeq").toInteger();
	m_savedAt = m_savedSeq >= m_seq ? QDateTime::currentDateTime() : QDateTime{};
	// Shared files: "shared:" paths of the project resolve to this project's library folder
	setLibrary(message.value("library").toArray());
	const QStringList missing = message.contains("mmp") ? missingFiles() : QStringList{};
	if (missing.isEmpty()) { return finishJoin(message); }
	// The project waits (and so do the transactions after it) until its shared files are here
	m_pendingJoin = message;
	if (!downloadMissing(missing))
	{
		m_pendingJoin.reset();
		QTimer::singleShot(0, this, [this] { disconnectFromServer(); });
	}
}


void CollabSession::finishJoin(const QJsonObject& message)
{
	m_pendingJoin.reset();
	if (message.contains("mmp"))
	{
		// Keep a local working copy of the shared project in this instance's workspace
		const QString dir = ConfigManager::inst()->workingDir() + "collab/";
		QDir{}.mkpath(dir);
		const QString file = dir + m_project + ".mmp";
		QFile f{file};
		if (!f.open(QIODevice::WriteOnly) || f.write(message.value("mmp").toString().toUtf8()) < 0)
		{
			return fail(tr("Could not write %1").arg(file));
		}
		f.close();
		// Window sizes and positions are private (decision D9), so the shared project has none: keep this
		// user's windows as they are instead of LMMS' defaults
		struct WindowState
		{
			QPointer<QWidget> content;
			QRect geometry;
			bool visible;
			bool maximized;
		};
		std::vector<WindowState> windows;
		auto gui = gui::getGUI();
		if (gui && gui->mainWindow())
		{
			for (QMdiSubWindow* w : gui->mainWindow()->workspace()->subWindowList())
			{
				windows.push_back({w->widget(), w->geometry(), w->isVisible(), w->isMaximized()});
			}
		}
		m_loadingSnapshot = true;
		Engine::getSong()->loadProject(file);
		m_loadingSnapshot = false;
		for (const WindowState& state : windows)
		{
			auto w = state.content ? qobject_cast<QMdiSubWindow*>(state.content->parentWidget()) : nullptr;
			if (!w) { continue; } // e.g. an instrument window of the previous song
			// Only windows that were open are shown (showing one has effects: an empty Piano Roll opens a clip)
			if (!state.visible)
			{
				w->hide();
				w->setGeometry(state.geometry);
			}
			else if (state.maximized) { w->showMaximized(); }
			else
			{
				w->showNormal();
				w->setGeometry(state.geometry);
			}
		}
		removeClipsNotIn(message.value("mmp").toString());
	}
	startTracking();
	if (message.contains("mmp"))
	{
		// LMMS connects automation clips to parameters by journal ids, which only its own saves share
		QDomDocument doc;
		if (doc.setContent(message.value("mmp").toString()))
		{
			resolveAutomation(doc.documentElement());
			m_structure = currentStructure(); // the automation clips' content as it is now
		}
	}
	setState(State::Live);
	if (!message.contains("mmp")) { shareProjectFiles(); } // we created it: files only we have must be shared
	processTxQueue(); // what happened while the files were downloading
}


void CollabSession::startTracking()
{
	stopTracking();

	// Every track at this moment is shared (also automation tracks, although only some of their changes
	// are synchronized so far)
	for (const TrackContainer* container : {static_cast<TrackContainer*>(Engine::getSong()),
		static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (Track* track : container->tracks())
		{
			m_structure.tracks.insert(track->collabId(), trackFields(track));
			m_structure.trackTypes.insert(track->collabId(), static_cast<int>(track->type()));
		}
	}
	m_structure = currentStructure();
	syncNoteTracking();

	ProjectJournal::setHook(this);
	m_structureTimer->start(StructureIntervalMs);
	refreshParamIndex();
	flushParams(); // takes the current values as baseline
	m_structure = currentStructure(); // automation clips name their parameters through the index
	m_trackStateTimer->start(250);

	// Loading another project replaces every shared object: the session cannot continue
	m_trackConnections.append(connect(Engine::getSong(), &Song::projectLoaded, this, [this] {
		if (!m_loadingSnapshot && m_state == State::Live)
		{
			fail(tr("Another project was loaded, so the collaboration session was closed."));
		}
	}));
}


void CollabSession::stopTracking()
{
	m_structureTimer->stop();
	m_trackStateTimer->stop();
	ProjectJournal::setHook(nullptr);
	for (const auto& c : m_trackConnections) { disconnect(c); }
	m_trackConnections.clear();
	m_trackers.clear();
}


void CollabSession::flushAll()
{
	if (m_state != State::Live) { return; }
	flushStructure();
	flushPlugins();
	flushParams();
	std::vector<MidiClip*> clips;
	for (const auto& [clip, tracker] : m_trackers) { clips.push_back(clip); }
	for (MidiClip* clip : clips) { flushClip(clip); }
}


// ------------------------------------------------------------------------------------------------
// Incoming transactions

void CollabSession::processTxQueue()
{
	if ((m_state != State::Live && m_state != State::Joining) || m_pendingJoin) { return; }
	if (localGestureInProgress())
	{
		if (!m_txQueueTimer->isActive()) { m_txQueueTimer->start(QueueRetryIntervalMs); }
		return;
	}
	while (!m_txQueue.isEmpty()) { applyTx(m_txQueue.takeFirst()); }
}


void CollabSession::applyTx(const QJsonObject& message)
{
	m_seq = message.value("seq").toInteger();

	if (message.value("clientId").toString() == m_clientId)
	{
		// Acknowledgement of our own transaction: our writes up to ctx are now ordered by the server
		const qint64 ctx = message.value("ctx").toInteger();
		m_ackedCtx = std::max(m_ackedCtx, ctx);
		emit syncStatusChanged();
		for (auto it = m_pending.begin(); it != m_pending.end();)
		{
			for (auto& fieldCtx : it.value())
			{
				if (fieldCtx <= ctx) { fieldCtx = 0; }
			}
			it = it.value() == PendingFields{} ? m_pending.erase(it) : std::next(it);
		}
		for (auto it = m_pendingStructure.begin(); it != m_pendingStructure.end();)
		{
			it = it.value() <= ctx ? m_pendingStructure.erase(it) : std::next(it);
		}
		paramsAcknowledged(ctx, m_seq);
		const QJsonArray sent = m_unacknowledgedOps.value(ctx);
		for (auto it = m_unacknowledgedOps.begin(); it != m_unacknowledgedOps.end() && it.key() <= ctx;)
		{
			it = m_unacknowledgedOps.erase(it);
		}
		if (const int rejected = message.value("rejected").toInt(); rejected > 0)
		{
			// The server did not take some of our changes: this model is no longer the shared project
			log(QString{"server rejected %1 op(s) of ctx %2: %3"}.arg(rejected).arg(ctx).arg(opsSummary(sent)));
			QTimer::singleShot(0, this, &CollabSession::resyncAfterRejection);
		}
		return;
	}

	const QJsonArray ops = message.value("ops").toArray();
	if (ops.isEmpty()) { return; }
	if (m_log)
	{
		log(QString{"recv seq %1 from %2: %3"}.arg(m_seq).arg(message.value("clientId").toString(), opsSummary(ops)));
	}

	// Send our own unsent edits first, so every baseline only holds synchronized state
	flushAll();

	const bool journalling = Engine::projectJournal()->isJournalling();
	Engine::projectJournal()->setJournalling(false);

	bool structureChanged = false;
	QJsonArray noteGroup;
	collab_id_t groupClip = 0;
	auto flushNoteGroup = [&] {
		if (!noteGroup.isEmpty()) { applyRemoteNoteOps(groupClip, noteGroup); }
		noteGroup = QJsonArray{};
	};
	for (const QJsonValue& value : ops)
	{
		const QJsonObject op = value.toObject();
		const QString type = op.value("op").toString();
		if (type == proto::op::ParamSet)
		{
			flushNoteGroup();
			applyRemoteParam(op, m_seq);
			continue;
		}
		if (type == proto::op::ParamLink)
		{
			flushNoteGroup();
			applyRemoteLink(op, m_seq);
			continue;
		}
		if (type == proto::op::LibraryAdd)
		{
			addLibraryFile(op.value("path").toString(), op.value("hash").toString(), op.value("size").toInteger(), true,
				op.value("by").toString().left(64));
			continue;
		}
		if (type == proto::op::TrackState || type == proto::op::MixerState || type == proto::op::ControllersState)
		{
			continue; // for the server's copy
		}
		if (type == proto::op::InstrumentSet || type == proto::op::EffectsSet)
		{
			flushNoteGroup();
			if (type == proto::op::InstrumentSet) { applyRemoteInstrument(op); }
			else { applyRemoteEffects(op); }
			continue;
		}
		if (type.startsWith("note."))
		{
			const collab_id_t clipId = proto::parseId(op.value("clip"));
			if (clipId != groupClip) { flushNoteGroup(); }
			groupClip = clipId;
			noteGroup.append(op);
			continue;
		}
		flushNoteGroup();
		applyRemoteStructureOp(op);
		structureChanged = true;
	}
	flushNoteGroup();

	// Everything local was flushed before, so the model now is the synchronized structure. This also
	// covers values derived from remote changes (e.g. a clip's length follows its notes): not echoed.
	m_structure = currentStructure();
	syncNoteTracking();
	if (structureChanged)
	{
		refreshParamIndex(); // tracks may have come or gone
		retryAutomationObjects(); // automated parameters of tracks that arrived in this transaction
		m_structure = currentStructure();
		Engine::getSong()->setModified();
	}
	if (!m_unresolvedLinks.isEmpty()) { retryLinks(); } // e.g. a Peak Controller effect that just arrived
	Engine::projectJournal()->setJournalling(journalling);
}


// ------------------------------------------------------------------------------------------------
// Notes

void CollabSession::trackClip(MidiClip* clip)
{
	if (m_trackers.find(clip) != m_trackers.end()) { return; }
	auto tracker = std::make_unique<ClipTracker>();
	tracker->timer.setSingleShot(true);
	connect(&tracker->timer, &QTimer::timeout, this, [this, clip] { flushClip(clip); });
	tracker->changed = connect(clip, &MidiClip::dataChanged, this, [this, clip] { onClipChanged(clip); });
	tracker->destroyed = connect(clip, &MidiClip::destroyedMidiClip, this, [this](MidiClip* c) {
		m_trackers.erase(c);
	});
	m_trackers[clip] = std::move(tracker);
}


void CollabSession::untrackClip(collab_id_t clipId)
{
	m_baselines.remove(clipId);
	if (MidiClip* clip = findMidiClip(clipId)) { m_trackers.erase(clip); }
	for (auto it = m_pending.begin(); it != m_pending.end();)
	{
		it = it.key().first == clipId ? m_pending.erase(it) : std::next(it);
	}
}


void CollabSession::syncNoteTracking()
{
	// The notes of every shared MIDI clip are synchronized; clips come and go with the structure
	for (auto it = m_structure.clips.cbegin(); it != m_structure.clips.cend(); ++it)
	{
		if (m_baselines.contains(it.key())) { continue; }
		if (MidiClip* clip = findMidiClip(it.key()))
		{
			m_baselines[it.key()] = currentNotes(*clip);
			trackClip(clip);
		}
	}
	for (const collab_id_t clipId : m_baselines.keys())
	{
		if (!m_structure.clips.contains(clipId)) { untrackClip(clipId); }
	}
}



void CollabSession::onClipChanged(MidiClip* clip)
{
	if (m_applyingRemote || m_state != State::Live) { return; }
	const auto it = m_trackers.find(clip);
	if (it != m_trackers.end() && !it->second->timer.isActive()) { it->second->timer.start(FlushIntervalMs); }
}


CollabSession::NoteMap CollabSession::currentNotes(const MidiClip& clip)
{
	NoteMap notes;
	for (const Note* note : clip.notes()) { notes.insert(note->collabId(), NoteState::of(*note)); }
	return notes;
}


void CollabSession::flushClip(MidiClip* clip)
{
	if (m_state != State::Live) { return; }
	if (const auto it = m_trackers.find(clip); it != m_trackers.end()) { it->second->timer.stop(); }

	const collab_id_t clipId = clip->collabId();
	if (!m_baselines.contains(clipId)) { return; }
	const QString clipIdStr = proto::idString(clipId);
	NoteMap& baseline = m_baselines[clipId];
	const NoteMap current = currentNotes(*clip);
	const qint64 ctx = m_nextCtx;
	QJsonArray ops;

	for (auto it = current.cbegin(); it != current.cend(); ++it)
	{
		const auto base = baseline.constFind(it.key());
		auto& pending = m_pending[{clipId, it.key()}];
		NoteValues values;
		for (int f = 0; f < NoteValues::FieldCount; ++f)
		{
			if (base == baseline.cend() || base->fields[f] != it->fields[f])
			{
				values.fields[f] = it->fields[f];
				pending[f] = ctx;
			}
		}
		if (values.isEmpty())
		{
			if (pending == PendingFields{}) { m_pending.remove({clipId, it.key()}); }
			continue;
		}
		record(Kind::Note, clipId, it.key(),
			base == baseline.cend() ? ObjectState{} : ObjectState{base->toJson()}, it->toJson());
		ops.append(QJsonObject{{"op", base == baseline.cend() ? proto::op::NoteAdd : proto::op::NoteSet},
			{"clip", clipIdStr}, {"id", proto::idString(it.key())}, {"v", values.toJson()}});
	}
	for (auto it = baseline.cbegin(); it != baseline.cend(); ++it)
	{
		if (current.contains(it.key())) { continue; }
		record(Kind::Note, clipId, it.key(), it->toJson(), std::nullopt);
		m_pending.remove({clipId, it.key()});
		ops.append(QJsonObject{{"op", proto::op::NoteRemove}, {"clip", clipIdStr}, {"id", proto::idString(it.key())}});
	}

	baseline = current;
	if (!ops.isEmpty()) { sendOps(ops); }
}


void CollabSession::applyRemoteNoteOps(collab_id_t clipId, const QJsonArray& ops)
{
	if (!m_baselines.contains(clipId)) { return; } // not a shared clip
	MidiClip* clip = findMidiClip(clipId);

	NoteMap& baseline = m_baselines[clipId];
	bool changed = false;
	m_applyingRemote = true;
	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		for (const QJsonValue& value : ops)
		{
			const QJsonObject op = value.toObject();
			const QString type = op.value("op").toString();
			const collab_id_t noteId = proto::parseId(op.value("id"));
			Note* note = clip ? clip->findNote(noteId) : nullptr;

			if (type == proto::op::NoteRemove)
			{
				baseline.remove(noteId);
				m_pending.remove({clipId, noteId});
				if (note) { clip->removeNote(note); changed = true; }
				continue;
			}

			const auto values = NoteValues::fromJson(op.value("v").toObject());
			if (!values) { continue; }
			const PendingFields pending = m_pending.value({clipId, noteId});

			if (type == proto::op::NoteAdd && !note && values->isComplete())
			{
				NoteState s;
				for (int f = 0; f < NoteValues::FieldCount; ++f) { s.fields[f] = *values->fields[f]; }
				if (clip)
				{
					writeNote(clip, noteId, s);
					note = clip->findNote(noteId);
					changed = true;
				}
				else { baseline.insert(noteId, s); }
			}
			else if (type == proto::op::NoteAdd || type == proto::op::NoteSet)
			{
				for (int f = 0; f < NoteValues::FieldCount; ++f)
				{
					const auto& v = values->fields[f];
					if (!v || pending[f] != 0) { continue; } // our unacknowledged write wins
					if (note)
					{
						NoteState s = NoteState::of(*note);
						s.fields[f] = *v;
						writeNote(clip, noteId, s);
						changed = true;
					}
					else if (baseline.contains(noteId)) { baseline[noteId].fields[f] = *v; }
				}
			}
			if (note) { baseline.insert(noteId, NoteState::of(*note)); }
		}
		if (clip && changed)
		{
			clip->rearrangeAllNotes();
			clip->updateLength();
		}
	}
	if (clip && changed)
	{
		emit clip->dataChanged();
		Engine::getSong()->setModified();
	}
	m_applyingRemote = false;
}


void CollabSession::writeNote(MidiClip* clip, collab_id_t id, const std::optional<NoteState>& target)
{
	Note* note = clip->findNote(id);
	if (!target)
	{
		if (note) { clip->removeNote(note); }
		return;
	}
	const auto& f = target->fields;
	if (!note)
	{
		Note n{TimePos{f[NoteValues::Len]}, TimePos{f[NoteValues::Pos]}, f[NoteValues::Key],
			static_cast<volume_t>(f[NoteValues::Vol]), static_cast<panning_t>(f[NoteValues::Pan])};
		n.setType(static_cast<Note::Type>(f[NoteValues::Type]));
		clip->addNote(n, false, id);
		return;
	}
	note->setKey(f[NoteValues::Key]);
	note->setPos(TimePos{f[NoteValues::Pos]});
	note->setLength(TimePos{f[NoteValues::Len]});
	note->setVolume(static_cast<volume_t>(f[NoteValues::Vol]));
	note->setPanning(static_cast<panning_t>(f[NoteValues::Pan]));
	note->setType(static_cast<Note::Type>(f[NoteValues::Type]));
}


// ------------------------------------------------------------------------------------------------
// Structure (tracks and clips of the Song Editor and the Pattern Editor, project notes)

bool CollabSession::syncsStructure(int trackType, bool inPatternEditor)
{
	// Tracks that can be created and removed by collaborators
	const bool content = trackType == static_cast<int>(Track::Type::Instrument)
		|| trackType == static_cast<int>(Track::Type::Sample) || trackType == static_cast<int>(Track::Type::Automation);
	return inPatternEditor ? content : content || trackType == static_cast<int>(Track::Type::Pattern);
}


QJsonObject CollabSession::trackFields(const Track* track)
{
	QJsonObject fields{{"name", track->name()}, {"muted", sharedMute(track)},
		{"color", track->color() ? track->color()->name() : QString{}}};
	// The mixer channel, by id: numbers change when channels are removed or moved
	const IntModel* channel = nullptr;
	if (auto t = dynamic_cast<const InstrumentTrack*>(track)) { channel = const_cast<InstrumentTrack*>(t)->mixerChannelModel(); }
	if (auto t = dynamic_cast<const SampleTrack*>(track)) { channel = const_cast<SampleTrack*>(t)->mixerChannelModel(); }
	const int index = channel ? channel->value() : -1;
	if (index >= 0 && index < static_cast<int>(Engine::mixer()->numChannels()))
	{
		fields.insert("channel", proto::idString(Engine::mixer()->mixerChannel(index)->collabId()));
	}
	return fields;
}


QJsonObject CollabSession::clipFields(const Clip* clip)
{
	QJsonObject fields{{"pos", clip->startPosition().getTicks()}, {"len", clip->length().getTicks()},
		{"off", clip->startTimeOffset().getTicks()}, {"name", clip->name()},
		{"color", clip->color() ? clip->color()->name() : QString{}}, {"muted", clip->isMuted()},
		{"autoresize", clip->getAutoResize()}};
	if (auto midiClip = dynamic_cast<const MidiClip*>(clip)) { fields.insert("steps", midiClip->stepCount()); }
	if (auto sampleClip = dynamic_cast<const SampleClip*>(clip)) { fields.insert("src", sampleClip->sampleFile()); }
	// In the Pattern Editor the position is the pattern the clip belongs to, not something to edit
	if (clip->getTrack() && clip->getTrack()->trackContainer() == Engine::patternStore()) { fields.remove("pos"); }
	return fields;
}


void CollabSession::applyTrackFields(Track* track, const QJsonObject& fields)
{
	if (fields.contains("name")) { track->setName(fields.value("name").toString()); }
	if (fields.contains("color"))
	{
		const QString color = fields.value("color").toString();
		track->setColor(color.isEmpty() ? std::nullopt : std::optional<QColor>{QColor{color}});
	}
	if (fields.contains("muted"))
	{
		const bool muted = fields.value("muted").toBool();
		// While this user has a (private) solo, only the mute to restore after the solo changes
		if (anySolo(track)) { track->setMutedBeforeSolo(muted); }
		else { track->setMuted(muted); }
	}
	if (fields.contains("channel"))
	{
		IntModel* channel = nullptr;
		if (auto t = dynamic_cast<InstrumentTrack*>(track)) { channel = t->mixerChannelModel(); }
		if (auto t = dynamic_cast<SampleTrack*>(track)) { channel = t->mixerChannelModel(); }
		const int index = mixerChannelIndex(proto::parseId(fields.value("channel")));
		if (channel && index >= 0)
		{
			// Without a GUI nothing else widens the selector's range to the channels that exist
			if (channel->maxValue() < index) { channel->setRange(0, Engine::mixer()->numChannels() - 1, 1); }
			channel->setValue(index);
		}
	}
}


void CollabSession::applyClipFields(Clip* clip, const QJsonObject& fields)
{
	if (fields.contains("src") && dynamic_cast<SampleClip*>(clip))
	{
		// Through its XML, so the clip keeps its length and offset (setSampleFile() resets them)
		QDomDocument doc;
		if (doc.setContent(serialize(clip)))
		{
			doc.documentElement().setAttribute("src", fields.value("src").toString());
			doc.documentElement().removeAttribute("data");
			clip->restoreState(doc.documentElement());
		}
	}
	if (fields.contains("autoresize")) { clip->setAutoResize(fields.value("autoresize").toBool()); }
	if (fields.contains("steps"))
	{
		if (auto midiClip = dynamic_cast<MidiClip*>(clip)) { midiClip->setStepCount(fields.value("steps").toInt()); }
	}
	if (fields.contains("off")) { clip->setStartTimeOffset(TimePos{fields.value("off").toInt()}); }
	if (fields.contains("pos")) { clip->movePosition(TimePos{fields.value("pos").toInt()}); }
	if (fields.contains("len")) { clip->changeLength(TimePos{fields.value("len").toInt()}); }
	if (fields.contains("name")) { clip->setName(fields.value("name").toString()); }
	if (fields.contains("color"))
	{
		const QString color = fields.value("color").toString();
		clip->setColor(color.isEmpty() ? std::nullopt : std::optional<QColor>{QColor{color}});
	}
	if (fields.contains("muted") && fields.value("muted").toBool() != clip->isMuted()) { clip->toggleMute(); }
}


QString CollabSession::serialize(Track* track)
{
	QDomDocument doc;
	QDomElement parent = doc.createElement("collab");
	doc.appendChild(parent);
	track->saveState(doc, parent);
	QDomElement element = parent.firstChildElement();
	for (const QString& tag : {QString{"instrumenttrack"}, QString{"sampletrack"}})
	{
		for (QDomElement e = element.firstChildElement(tag); !e.isNull(); e = e.nextSiblingElement(tag))
		{
			for (const QString& a : WindowAttributes) { e.removeAttribute(a); }
		}
	}
	// The pattern track with index 0 also saves the whole Pattern Editor; that is shared separately
	for (QDomElement p = element.firstChildElement("patterntrack"); !p.isNull(); p = p.nextSiblingElement("patterntrack"))
	{
		for (QDomElement c = p.firstChildElement("trackcontainer"); !c.isNull(); c = p.firstChildElement("trackcontainer"))
		{
			p.removeChild(c);
		}
	}
	annotateAutomation(element);
	const bool muted = sharedMute(track);
	element.setAttribute("solo", 0);
	element.setAttribute("muted", muted ? 1 : 0);
	element.setAttribute("mutedBeforeSolo", muted ? 1 : 0);
	return elementToString(element);
}


QString CollabSession::serialize(Clip* clip)
{
	QDomDocument doc;
	QDomElement parent = doc.createElement("collab");
	doc.appendChild(parent);
	clip->saveState(doc, parent);
	annotateAutomation(parent.firstChildElement());
	return elementToString(parent.firstChildElement());
}


Clip* CollabSession::createClipFromXml(Track* track, const QString& xml)
{
	QDomDocument doc;
	if (!doc.setContent(xml)) { return nullptr; }
	Clip* clip = nullptr;
	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		clip = track->createClip(TimePos{0});
		clip->restoreState(doc.documentElement());
	}
	instance()->resolveAutomation(doc.documentElement());
	return clip;
}


namespace
{

//! The editor showing a track container, if there is a GUI
gui::TrackContainerView* editorFor(const TrackContainer* container)
{
	auto gui = gui::getGUI();
	if (!gui) { return nullptr; }
	if (container == Engine::getSong() && gui->songEditor()) { return gui->songEditor()->m_editor; }
	if (container == Engine::patternStore() && gui->patternEditor()) { return gui->patternEditor()->m_editor; }
	return nullptr;
}

gui::TrackView* viewOf(gui::TrackContainerView* editor, const Track* track)
{
	if (!editor) { return nullptr; }
	for (gui::TrackView* view : editor->trackViews())
	{
		if (view->getTrack() == track) { return view; }
	}
	return nullptr;
}

//! LMMS creates the view of a new track later (queued signal). Operations that follow in the same
//! transaction (e.g. its position) need it now, and the editor's views in the same order as the tracks.
void ensureViews(TrackContainer* container)
{
	auto editor = editorFor(container);
	if (!editor) { return; }
	for (Track* track : container->tracks())
	{
		if (!viewOf(editor, track)) { editor->createTrackView(track); }
	}
}

} // namespace


void CollabSession::removeTrack(Track* track)
{
	// Same path as the editor's "remove track", so its view and windows go away properly
	auto editor = editorFor(track->trackContainer());
	if (gui::TrackView* view = viewOf(editor, track))
	{
		editor->deleteTrackView(view);
		return;
	}
	auto guard = Engine::audioEngine()->requestChangesGuard();
	delete track;
}


void CollabSession::removeClip(Clip* clip)
{
	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		clip->getTrack()->removeClip(clip);
	}
	delete clip; // its views close themselves (destroyedClip)
}


void CollabSession::reorderTracks(TrackContainer* container, const QList<collab_id_t>& order)
{
	ensureViews(container);
	auto indexOf = [container](const Track* t) {
		const auto& tracks = container->tracks();
		return static_cast<int>(std::find(tracks.begin(), tracks.end(), t) - tracks.begin());
	};
	auto editor = editorFor(container);
	// One step, exactly like dragging a track by one position (swaps patterns of two pattern tracks)
	auto step = [&](Track* track, int to) {
		if (gui::TrackView* view = viewOf(editor, track))
		{
			editor->moveTrackView(view, to);
			return;
		}
		PatternTrack::swapPatternTracks(track, container->tracks()[to]);
		container->moveTrack(track, to);
	};

	std::vector<Track*> wanted;
	for (const collab_id_t id : order)
	{
		Track* t = findTrack(id);
		if (t && t->trackContainer() == container) { wanted.push_back(t); }
	}
	// The listed tracks take the positions they occupy now, in the wanted order; others stay where they are
	std::vector<int> positions;
	for (Track* t : wanted) { positions.push_back(indexOf(t)); }
	std::sort(positions.begin(), positions.end());

	// Every step moves a track by one; should views and tracks ever disagree, give up instead of hanging
	int stepsLeft = static_cast<int>(container->tracks().size() * container->tracks().size()) + 16;
	for (std::size_t k = 0; k < wanted.size() && stepsLeft > 0; ++k)
	{
		while (indexOf(wanted[k]) > positions[k] && stepsLeft-- > 0) { step(wanted[k], indexOf(wanted[k]) - 1); }
		while (indexOf(wanted[k]) < positions[k] && stepsLeft-- > 0) { step(wanted[k], indexOf(wanted[k]) + 1); }
	}
	if (stepsLeft <= 0) { instance()->log("track order: views and tracks disagree, order not fully applied"); }
}


std::optional<QString> CollabSession::currentNotes()
{
	auto gui = gui::getGUI();
	if (!gui || !gui->getProjectNotes()) { return std::nullopt; }
	return gui->getProjectNotes()->html();
}


CollabSession::Structure CollabSession::currentStructure() const
{
	Structure s;
	auto addTracks = [&](const TrackContainer* container, bool inPatternEditor) {
		for (Track* track : container->tracks())
		{
			const collab_id_t id = track->collabId();
			const int type = static_cast<int>(track->type());
			if (!m_structure.tracks.contains(id) && !syncsStructure(type, inPatternEditor)) { continue; }
			s.tracks.insert(id, trackFields(track));
			s.trackTypes.insert(id, type);
			(inPatternEditor ? s.patternOrder : s.order).append(id);
			if (inPatternEditor) { s.patternEditorTracks.insert(id); }
			for (const Clip* clip : track->getClips())
			{
				QJsonObject fields = clipFields(clip);
				// What an automation clip holds, compared as a whole (hidden field: never part of clip.set)
				if (auto automation = dynamic_cast<const AutomationClip*>(clip)) { fields.insert("_a", automationSignature(automation)); }
				s.clips.insert(clip->collabId(), Structure::ClipInfo{id, fields});
			}
		}
	};
	addTracks(Engine::getSong(), false);
	addTracks(Engine::patternStore(), true);
	s.notes = currentNotes();
	addMixerStructure(s);
	addControllerStructure(s);
	return s;
}


void CollabSession::flushStructure()
{
	if (m_state != State::Live) { return; }
	Structure current = currentStructure();
	const qint64 ctx = m_nextCtx;
	QJsonArray ops;

	auto markPending = [&](char kind, collab_id_t id, const QJsonObject& fields) {
		for (auto it = fields.begin(); it != fields.end(); ++it) { m_pendingStructure[pendingKey(kind, id, it.key())] = ctx; }
	};
	auto clipState = [](collab_id_t clipId, QJsonObject fields) {
		// Clips carry a signature of their notes, so undo never removes a clip whose content changed
		if (const MidiClip* clip = findMidiClip(clipId)) { fields.insert("_n", notesSignature(currentNotes(*clip))); }
		return fields;
	};
	auto isNew = [this](collab_id_t trackId) { return !m_structure.tracks.contains(trackId); };

	// Controllers and the mixer first: tracks sent below may already use new ones
	flushControllers(current, ops, ctx);
	flushMixer(current, ops, ctx);

	// New tracks that use files only this computer has wait until those are shared: out of this round
	for (const TrackContainer* container : {static_cast<TrackContainer*>(Engine::getSong()),
		static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (Track* track : container->tracks())
		{
			const collab_id_t id = track->collabId();
			if (!current.tracks.contains(id) || !isNew(id) || readyToSend(serialize(track))) { continue; }
			current.tracks.remove(id);
			current.trackTypes.remove(id);
			current.order.removeAll(id);
			current.patternOrder.removeAll(id);
			current.patternEditorTracks.remove(id);
			for (auto it = current.clips.begin(); it != current.clips.end();)
			{
				it = it->track == id ? current.clips.erase(it) : std::next(it);
			}
		}
	}

	// New Song Editor tracks are sent complete (instrument, settings, clips); a new pattern also carries
	// its content: its clip in every Pattern Editor track that is already shared
	for (Track* track : Engine::getSong()->tracks())
	{
		const collab_id_t id = track->collabId();
		if (!current.tracks.contains(id) || !isNew(id)) { continue; }
		if (auto pattern = dynamic_cast<PatternTrack*>(track))
		{
			QJsonArray clips;
			for (Track* editorTrack : Engine::patternStore()->tracks())
			{
				const auto& trackClips = editorTrack->getClips();
				const auto index = static_cast<std::size_t>(pattern->patternIndex());
				if (isNew(editorTrack->collabId()) || index >= trackClips.size()) { continue; }
				clips.append(QJsonObject{{"track", proto::idString(editorTrack->collabId())},
					{"xml", serialize(trackClips[index])}});
			}
			ops.append(QJsonObject{{"op", proto::op::PatternAdd}, {"xml", serialize(track)}, {"clips", clips}});
		}
		else
		{
			ops.append(QJsonObject{{"op", proto::op::TrackAdd}, {"container", proto::SongContainer},
				{"index", -1}, {"xml", serialize(track)}});
		}
	}
	// New Pattern Editor tracks, with their clip for every pattern
	for (Track* track : Engine::patternStore()->tracks())
	{
		const collab_id_t id = track->collabId();
		if (!current.tracks.contains(id) || !isNew(id)) { continue; }
		ops.append(QJsonObject{{"op", proto::op::TrackAdd}, {"container", proto::PatternContainer},
			{"index", -1}, {"xml", serialize(track)}});
	}

	// Sample clips with a file only this computer has wait until it is shared
	for (auto it = current.clips.begin(); it != current.clips.end();)
	{
		const auto old = m_structure.clips.constFind(it.key());
		const bool changedFile = it->fields.contains("src")
			&& (old == m_structure.clips.cend() || old->fields.value("src") != it->fields.value("src"));
		if (!changedFile || isNew(it->track) || readyToSend(serialize(findClip(it.key())))) { ++it; continue; }
		if (old == m_structure.clips.cend()) { it = current.clips.erase(it); }
		else
		{
			it->fields.insert("src", old->fields.value("src"));
			++it;
		}
	}

	// Clips: Song Editor clips come and go on their own; Pattern Editor clips only change (they are
	// created and removed together with patterns and tracks)
	for (auto it = current.clips.cbegin(); it != current.clips.cend(); ++it)
	{
		const collab_id_t clipId = it.key();
		const collab_id_t trackId = it->track;
		if (isNew(trackId)) { continue; }
		const auto old = m_structure.clips.constFind(clipId);
		if (old == m_structure.clips.cend())
		{
			if (current.patternEditorTracks.contains(trackId)) { continue; } // part of a new pattern
			ops.append(QJsonObject{{"op", proto::op::ClipAdd}, {"track", proto::idString(trackId)},
				{"xml", serialize(findClip(clipId))}});
			record(Kind::Clip, trackId, clipId, std::nullopt, clipState(clipId, it->fields));
			continue;
		}
		if (it->fields.contains("_a") && old->fields.value("_a") != it->fields.value("_a"))
		{
			if (auto automation = dynamic_cast<AutomationClip*>(findClip(clipId)))
			{
				QJsonObject op = automationContent(automation);
				op.insert("op", proto::op::AutomationSet);
				op.insert("clip", proto::idString(clipId));
				ops.append(op);
				m_pendingStructure[pendingKey('a', clipId, "content")] = ctx;
				record(Kind::Automation, clipId, clipId, QJsonObject{{"c", old->fields.value("_a")}},
					QJsonObject{{"c", it->fields.value("_a")}});
			}
		}
		const QJsonObject changed = changedFields(old->fields, it->fields);
		if (changed.isEmpty()) { continue; }
		ops.append(QJsonObject{{"op", proto::op::ClipSet}, {"id", proto::idString(clipId)}, {"v", changed}});
		markPending('c', clipId, changed);
		record(Kind::Clip, trackId, clipId, clipState(clipId, old->fields), clipState(clipId, it->fields));
	}
	for (auto it = m_structure.clips.cbegin(); it != m_structure.clips.cend(); ++it)
	{
		// Clips of removed tracks and Pattern Editor clips go away with their track or pattern
		if (current.clips.contains(it.key()) || !current.tracks.contains(it->track)
			|| m_structure.patternEditorTracks.contains(it->track))
		{
			continue;
		}
		ops.append(QJsonObject{{"op", proto::op::ClipRemove}, {"id", proto::idString(it.key())}});
		QJsonObject before = it->fields;
		if (m_baselines.contains(it.key()))
		{
			// The clip is gone; its last synchronized notes stand for its content
			before.insert("_n", notesSignature(m_baselines.value(it.key())));
		}
		record(Kind::Clip, it->track, it.key(), before, std::nullopt);
	}

	// Track changes
	for (auto it = current.tracks.cbegin(); it != current.tracks.cend(); ++it)
	{
		const auto old = m_structure.tracks.constFind(it.key());
		if (old == m_structure.tracks.cend()) { continue; }
		const QJsonObject changed = changedFields(*old, *it);
		if (changed.isEmpty()) { continue; }
		ops.append(QJsonObject{{"op", proto::op::TrackSet}, {"id", proto::idString(it.key())}, {"v", changed}});
		markPending('t', it.key(), changed);
		record(Kind::Track, 0, it.key(), *old, *it);
	}
	for (auto it = m_structure.tracks.cbegin(); it != m_structure.tracks.cend(); ++it)
	{
		if (current.tracks.contains(it.key())) { continue; }
		const int type = m_structure.trackTypes.value(it.key());
		const bool inPatternEditor = m_structure.patternEditorTracks.contains(it.key());
		if (!syncsStructure(type, inPatternEditor)) { continue; } // e.g. automation: just stops being shared
		const bool pattern = type == static_cast<int>(Track::Type::Pattern);
		ops.append(QJsonObject{{"op", pattern ? proto::op::PatternRemove : proto::op::TrackRemove},
			{"id", proto::idString(it.key())}});
	}

	// Order (after additions and removals, so every listed track exists for the others)
	auto orderOp = [&](const QList<collab_id_t>& order, const char* container) {
		QJsonArray ids;
		for (const collab_id_t id : order) { ids.append(proto::idString(id)); }
		ops.append(QJsonObject{{"op", proto::op::TrackOrder}, {"container", container}, {"ids", ids}});
	};
	if (current.order != m_structure.order) { orderOp(current.order, proto::SongContainer); }
	if (current.patternOrder != m_structure.patternOrder) { orderOp(current.patternOrder, proto::PatternContainer); }

	// Project notes: the whole text, at most twice per second while someone types
	if (current.notes && current.notes != m_structure.notes)
	{
		if (!m_notesSent.isValid() || m_notesSent.elapsed() >= 500)
		{
			ops.append(QJsonObject{{"op", proto::op::NotesSet}, {"text", *current.notes}});
			m_pendingStructure[pendingKey('n', 0, "text")] = ctx;
			m_notesSent.start();
		}
		else { current.notes = m_structure.notes; } // try again at the next tick
	}

	m_structure = current;
	syncNoteTracking();
	if (!ops.isEmpty()) { sendOps(ops); }
}


void CollabSession::addRemotePattern(const QJsonObject& op)
{
	QDomDocument doc;
	if (!doc.setContent(op.value("xml").toString()) || findTrack(proto::parseId(doc.documentElement().attribute("cid"))))
	{
		return;
	}
	// Creating a pattern selects it in the Pattern Editor; which pattern this user looks at is private
	const int shownPattern = Engine::patternStore()->currentPattern();
	auto pattern = dynamic_cast<PatternTrack*>(Track::create(doc.documentElement(), Engine::getSong()));
	if (!pattern) { return; }
	ensureViews(Engine::getSong());
	const auto index = static_cast<std::size_t>(pattern->patternIndex());

	// The new pattern's clips were just created with local ids: take over the author's clips
	for (const QJsonValue& value : op.value("clips").toArray())
	{
		const QJsonObject entry = value.toObject();
		Track* editorTrack = findTrack(proto::parseId(entry.value("track")));
		if (!editorTrack || editorTrack->trackContainer() != Engine::patternStore()) { continue; }
		const auto& clips = editorTrack->getClips();
		QDomDocument clipDoc;
		if (index >= clips.size() || !clipDoc.setContent(entry.value("xml").toString())) { continue; }
		Clip* clip = clips[index];
		const TimePos position = clip->startPosition(); // the position is the pattern here
		clip->restoreState(clipDoc.documentElement());
		clip->movePosition(position);
		resolveAutomation(clipDoc.documentElement());
	}
	Engine::patternStore()->setCurrentPattern(shownPattern);
	Engine::patternStore()->updateComboBox();
}


void CollabSession::applyRemoteStructureOp(const QJsonObject& op)
{
	const QString type = op.value("op").toString();
	auto without = [this](char kind, collab_id_t id, QJsonObject fields) {
		for (const QString& key : fields.keys())
		{
			// our unacknowledged write of this field wins
			if (m_pendingStructure.contains(pendingKey(kind, id, key))) { fields.remove(key); }
		}
		return fields;
	};
	auto containerNamed = [](const QString& name) -> TrackContainer* {
		if (name == proto::SongContainer) { return Engine::getSong(); }
		if (name == proto::PatternContainer) { return Engine::patternStore(); }
		return nullptr;
	};

	m_applyingRemote = true;
	if (type.startsWith("mixer.")) { applyRemoteMixerOp(op); }
	else if (type.startsWith("controller.")) { applyRemoteControllerOp(op); }
	else if (type == proto::op::TrackAdd)
	{
		QDomDocument doc;
		TrackContainer* container = containerNamed(op.value("container").toString());
		if (container && doc.setContent(op.value("xml").toString())
			&& !findTrack(proto::parseId(doc.documentElement().attribute("cid"))))
		{
			Track::create(doc.documentElement(), container);
			ensureViews(container);
			resolveAutomation(doc.documentElement());
		}
	}
	else if (type == proto::op::AutomationSet)
	{
		const collab_id_t id = proto::parseId(op.value("clip"));
		auto clip = dynamic_cast<AutomationClip*>(findClip(id));
		// our unacknowledged content wins
		if (clip && !m_pendingStructure.contains(pendingKey('a', id, "content"))) { applyAutomationContent(clip, op); }
	}
	else if (type == proto::op::PatternAdd) { addRemotePattern(op); }
	else if (type == proto::op::TrackRemove || type == proto::op::PatternRemove)
	{
		if (Track* track = findTrack(proto::parseId(op.value("id")))) { removeTrack(track); }
	}
	else if (type == proto::op::TrackSet)
	{
		const collab_id_t id = proto::parseId(op.value("id"));
		if (Track* track = findTrack(id)) { applyTrackFields(track, without('t', id, op.value("v").toObject())); }
	}
	else if (type == proto::op::TrackOrder)
	{
		if (TrackContainer* container = containerNamed(op.value("container").toString()))
		{
			QList<collab_id_t> order;
			for (const QJsonValue& v : op.value("ids").toArray()) { order.append(proto::parseId(v)); }
			reorderTracks(container, order);
		}
	}
	else if (type == proto::op::ClipAdd)
	{
		Track* track = findTrack(proto::parseId(op.value("track")));
		QDomDocument doc;
		if (track && doc.setContent(op.value("xml").toString())
			&& !findClip(proto::parseId(doc.documentElement().attribute("cid"))))
		{
			createClipFromXml(track, op.value("xml").toString());
		}
	}
	else if (type == proto::op::ClipRemove)
	{
		const collab_id_t id = proto::parseId(op.value("id"));
		if (Clip* clip = findClip(id))
		{
			untrackClip(id);
			removeClip(clip);
		}
	}
	else if (type == proto::op::ClipSet)
	{
		const collab_id_t id = proto::parseId(op.value("id"));
		if (Clip* clip = findClip(id)) { applyClipFields(clip, without('c', id, op.value("v").toObject())); }
	}
	else if (type == proto::op::NotesSet)
	{
		auto gui = gui::getGUI();
		if (gui && gui->getProjectNotes() && !m_pendingStructure.contains(pendingKey('n', 0, "text")))
		{
			gui->getProjectNotes()->setHtmlKeepingCursor(op.value("text").toString());
		}
	}
	m_applyingRemote = false;
}


// ------------------------------------------------------------------------------------------------
// Undo / redo (decision D4)

bool CollabSession::isShared(JournallingObject* jo) const
{
	if (auto clip = dynamic_cast<Clip*>(jo))
	{
		return m_structure.clips.contains(clip->collabId()) || m_baselines.contains(clip->collabId());
	}
	if (auto track = dynamic_cast<Track*>(jo)) { return m_structure.tracks.contains(track->collabId()); }
	if (auto model = dynamic_cast<AutomatableModel*>(jo)) { return paramKeyOf(model).has_value(); }
	return false;
}


std::uint64_t CollabSession::checkPointAdded(JournallingObject* jo)
{
	if (m_state != State::Live || !isShared(jo)) { return 0; }

	// Changes made before this checkpoint belong to earlier gestures
	flushAll();
	const std::uint64_t token = m_nextGesture++;
	Gesture& gesture = m_gestures[token];
	if (auto clip = dynamic_cast<Clip*>(jo))
	{
		m_openGesture[{static_cast<int>(Kind::Clip), clip->collabId()}] = token;
		gesture.clipXml.insert(clip->collabId(), serialize(clip));
	}
	else if (auto track = dynamic_cast<Track*>(jo))
	{
		m_openGesture[{static_cast<int>(Kind::Track), track->collabId()}] = token;
		for (Clip* clip : track->getClips())
		{
			// The track's gesture now collects the changes of all its clips (e.g. moving a selection)
			m_openGesture.remove({static_cast<int>(Kind::Clip), clip->collabId()});
			gesture.clipXml.insert(clip->collabId(), serialize(clip));
		}
	}
	else if (auto model = dynamic_cast<AutomatableModel*>(jo))
	{
		// A knob, slider, button or combo box: the gesture collects this parameter's changes
		m_openParamGesture[*paramKeyOf(model)] = token;
	}
	while (m_gestures.size() > 1000) { m_gestures.erase(m_gestures.begin()); } // the journal keeps 100 anyway
	return token;
}


bool CollabSession::restore(JournallingObject* jo, std::uint64_t token, bool undo)
{
	if (m_state != State::Live || !isShared(jo)) { return false; }
	// Never restore an old snapshot of a shared object: it would also revert other users' changes.
	flushAll();
	if (const auto g = m_gestures.find(token); token != 0 && g != m_gestures.end())
	{
		for (auto it = m_openGesture.begin(); it != m_openGesture.end();)
		{
			it = it.value() == token ? m_openGesture.erase(it) : std::next(it);
		}
		for (auto it = m_openParamGesture.begin(); it != m_openParamGesture.end();)
		{
			it = it.value() == token ? m_openParamGesture.erase(it) : std::next(it);
		}
		replayGesture(g->second, undo);
	}
	return true;
}


void CollabSession::restored(JournallingObject*)
{
	// A snapshot of something that is not shared was restored; changes to shared objects, if any, are
	// picked up by the regular comparison with the baselines.
}


CollabSession::Gesture* CollabSession::openGestureFor(Kind kind, collab_id_t parent, collab_id_t id,
	const QString& path)
{
	if (kind == Kind::Param)
	{
		const auto open = m_openParamGesture.constFind({parent, path});
		if (open == m_openParamGesture.cend()) { return nullptr; }
		const auto g = m_gestures.find(*open);
		return g != m_gestures.end() ? &g->second : nullptr;
	}
	auto find = [this](Kind k, collab_id_t objectId) -> Gesture* {
		const auto open = m_openGesture.constFind({static_cast<int>(k), objectId});
		if (open == m_openGesture.cend()) { return nullptr; }
		const auto g = m_gestures.find(*open);
		return g != m_gestures.end() ? &g->second : nullptr;
	};
	switch (kind)
	{
	case Kind::Track: return find(Kind::Track, id);
	case Kind::Clip:
		if (Gesture* g = find(Kind::Clip, id)) { return g; }
		return find(Kind::Track, parent);
	case Kind::Note:
	case Kind::Automation:
		if (Gesture* g = find(Kind::Clip, parent)) { return g; }
		if (const Clip* clip = findClip(parent)) { return find(Kind::Track, clip->getTrack()->collabId()); }
		return nullptr;
	case Kind::Param: break;
	}
	return nullptr;
}


void CollabSession::record(Kind kind, collab_id_t parent, collab_id_t id, const ObjectState& before,
	const ObjectState& after, const QString& path)
{
	if (!m_recordGestures) { return; }
	Gesture* gesture = openGestureFor(kind, parent, id, path);
	if (!gesture) { return; }
	const ObjectKey key{kind, parent, id, path};
	if (!gesture->before.contains(key)) { gesture->before.insert(key, before); }
	gesture->after.insert(key, after);
}


QString CollabSession::notesSignature(const NoteMap& notes)
{
	QList<collab_id_t> ids = notes.keys();
	std::sort(ids.begin(), ids.end());
	QString signature;
	for (const collab_id_t id : ids)
	{
		signature += proto::idString(id);
		for (const int v : notes.value(id).fields) { signature += QString::number(v) + ','; }
	}
	return signature;
}


CollabSession::ObjectState CollabSession::currentState(const ObjectKey& key) const
{
	switch (key.kind)
	{
	case Kind::Track:
		if (const Track* track = findTrack(key.id)) { return trackFields(track); }
		return std::nullopt;
	case Kind::Clip:
		if (const Clip* clip = findClip(key.id))
		{
			QJsonObject fields = clipFields(clip);
			if (auto midiClip = dynamic_cast<const MidiClip*>(clip))
			{
				fields.insert("_n", notesSignature(currentNotes(*midiClip)));
			}
			if (auto automation = dynamic_cast<const AutomationClip*>(clip)) { fields.insert("_a", automationSignature(automation)); }
			return fields;
		}
		return std::nullopt;
	case Kind::Automation:
		if (auto automation = dynamic_cast<const AutomationClip*>(findClip(key.id)))
		{
			return QJsonObject{{"c", automationSignature(automation)}};
		}
		return std::nullopt;
	case Kind::Note:
		if (const MidiClip* clip = findMidiClip(key.parent))
		{
			if (const Note* note = clip->findNote(key.id)) { return NoteState::of(*note).toJson(); }
		}
		return std::nullopt;
	case Kind::Param:
		// findParam() may rebuild the parameter index, which does not change the session's state
		if (const AutomatableModel* model = const_cast<CollabSession*>(this)->findParam({key.parent, key.path}))
		{
			return QJsonObject{{"v", model->value<float>()}};
		}
		return std::nullopt;
	}
	return std::nullopt;
}


void CollabSession::replayGesture(Gesture& gesture, bool undo)
{
	// "mine" is what this user left (for redo: what they had before undoing); "target" what to go back to
	const auto& mineStates = undo ? gesture.after : gesture.before;
	const auto& targetStates = undo ? gesture.before : gesture.after;
	QSet<MidiClip*> touchedClips;

	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		// Clips before notes: a note can only come back into a clip that exists
		for (const Kind kind : {Kind::Param, Kind::Track, Kind::Clip, Kind::Automation, Kind::Note})
		{
			for (auto it = mineStates.cbegin(); it != mineStates.cend(); ++it)
			{
				const ObjectKey& key = it.key();
				if (key.kind != kind) { continue; }
				const ObjectState& mine = it.value();
				const ObjectState target = targetStates.value(key);
				if (mine == target) { continue; }
				const ObjectState current = currentState(key);

				if (!mine)
				{
					// This user removed it: bring it back, unless it exists again by now
					if (current || !target) { continue; }
					if (key.kind == Kind::Clip)
					{
						Track* track = findTrack(key.parent);
						const QString xml = gesture.clipXml.value(key.id);
						if (!track || xml.isEmpty()) { continue; }
						if (Clip* clip = createClipFromXml(track, xml))
						{
							applyClipFields(clip, withoutPrivateKeys(*target));
						}
					}
					else if (key.kind == Kind::Note)
					{
						if (MidiClip* clip = findMidiClip(key.parent))
						{
							writeNote(clip, key.id, NoteState::fromJson(*target));
							touchedClips.insert(clip);
						}
					}
				}
				else if (!target)
				{
					// This user created it: remove it only if nobody changed it since
					if (current != mine) { continue; }
					if (key.kind == Kind::Clip)
					{
						if (Clip* clip = findClip(key.id))
						{
							gesture.clipXml.insert(key.id, serialize(clip)); // for redo
							untrackClip(key.id);
							removeClip(clip);
						}
					}
					else if (key.kind == Kind::Note)
					{
						if (MidiClip* clip = findMidiClip(key.parent))
						{
							writeNote(clip, key.id, std::nullopt);
							touchedClips.insert(clip);
						}
					}
				}
				else if (current)
				{
					// Revert each field this user changed, unless someone changed that field afterwards
					QJsonObject revert;
					for (auto f = mine->begin(); f != mine->end(); ++f)
					{
						if (f.key().startsWith('_')) { continue; }
						if (f.value() != target->value(f.key()) && current->value(f.key()) == f.value())
						{
							revert.insert(f.key(), target->value(f.key()));
						}
					}
					if (revert.isEmpty()) { continue; }
					if (key.kind == Kind::Param)
					{
						if (AutomatableModel* model = findParam({key.parent, key.path}))
						{
							model->setValue(static_cast<float>(revert.value("v").toDouble()));
						}
					}
					else if (key.kind == Kind::Track) { applyTrackFields(findTrack(key.id), revert); }
					else if (key.kind == Kind::Clip) { applyClipFields(findClip(key.id), revert); }
					else if (key.kind == Kind::Automation)
					{
						if (auto automation = dynamic_cast<AutomationClip*>(findClip(key.id)))
						{
							applyAutomationContent(automation,
								QJsonDocument::fromJson(revert.value("c").toString().toUtf8()).object());
						}
					}
					else if (MidiClip* clip = findMidiClip(key.parent))
					{
						QJsonObject next = *current;
						for (auto f = revert.begin(); f != revert.end(); ++f) { next.insert(f.key(), f.value()); }
						writeNote(clip, key.id, NoteState::fromJson(next));
						touchedClips.insert(clip);
					}
				}
				// else: someone else removed it meanwhile; leave it removed
			}
		}
		for (MidiClip* clip : touchedClips)
		{
			clip->rearrangeAllNotes();
			clip->updateLength();
		}
	}
	for (MidiClip* clip : touchedClips) { emit clip->dataChanged(); }

	// Send the result right away; it is the undo itself, not a new gesture to record
	m_recordGestures = false;
	flushAll();
	m_recordGestures = true;
}


// ------------------------------------------------------------------------------------------------

Clip* CollabSession::findClip(collab_id_t clipId)
{
	return const_cast<Clip*>(static_cast<const Clip*>(findOwner(IdScope::Clip, clipId)));
}


MidiClip* CollabSession::findMidiClip(collab_id_t clipId)
{
	return dynamic_cast<MidiClip*>(findClip(clipId));
}


Track* CollabSession::findTrack(collab_id_t trackId)
{
	return const_cast<Track*>(static_cast<const Track*>(findOwner(IdScope::Track, trackId)));
}

} // namespace lmms::collab
