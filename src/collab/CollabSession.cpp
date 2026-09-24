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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPointer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternStore.h"
#include "Song.h"

namespace lmms::collab
{

namespace
{
constexpr int FlushIntervalMs = 50;      // local changes are sent at most ~20 times per second
constexpr int QueueRetryIntervalMs = 30; // how often to check whether a mouse gesture has ended

using proto::NoteValues;
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
	m_txQueueTimer(new QTimer(this))
{
	m_txQueueTimer->setSingleShot(true);
	connect(m_txQueueTimer, &QTimer::timeout, this, &CollabSession::processTxQueue);
}


CollabSession::~CollabSession()
{
	stopTracking();
}


void CollabSession::connectToServer(const QString& host, quint16 port, const QString& user,
	const QString& project, JoinMode mode)
{
	disconnectFromServer();
	m_user = user;
	m_project = project;
	m_joinMode = mode;
	m_seq = 0;
	m_nextCtx = 1;

	m_socket = std::make_unique<QTcpSocket>();
	connect(m_socket.get(), &QTcpSocket::connected, this, [this] {
		send({{"t", proto::msg::Hello}, {"proto", proto::Version}, {"user", m_user}});
	});
	connect(m_socket.get(), &QTcpSocket::readyRead, this, &CollabSession::onReadyRead);
	connect(m_socket.get(), &QTcpSocket::errorOccurred, this, [this] {
		fail(tr("Connection error: %1").arg(m_socket ? m_socket->errorString() : QString{}));
	});
	setState(State::Connecting);
	m_socket->connectToHost(host, port);
}


void CollabSession::disconnectFromServer()
{
	stopTracking();
	m_baselines.clear();
	m_pending.clear();
	m_gestures.clear();
	m_openGesture.clear();
	m_txQueue.clear();
	m_txQueueTimer->stop();
	m_decoder = proto::FrameDecoder{};
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


void CollabSession::setState(State state)
{
	if (m_state == state) { return; }
	m_state = state;
	emit stateChanged();
}


void CollabSession::fail(const QString& message)
{
	const bool wasActive = m_state != State::Disconnected;
	disconnectFromServer();
	if (wasActive) { emit errorOccurred(message); }
}


void CollabSession::send(const QJsonObject& message)
{
	if (m_socket) { m_socket->write(proto::encodeJsonFrame(message)); }
}


void CollabSession::onReadyRead()
{
	m_decoder.append(m_socket->readAll());
	proto::FrameType type;
	QByteArray payload;
	while (m_socket && m_decoder.next(type, payload))
	{
		const auto message = type == proto::FrameType::Json ? proto::parseMessage(payload) : std::nullopt;
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
		if (!dir.isValid() || !Engine::getSong()->saveProjectFile(file) || !f.open(QIODevice::ReadOnly))
		{
			return fail(tr("Could not serialize the current song."));
		}
		send({{"t", proto::msg::Create}, {"project", m_project}, {"mmp", QString::fromUtf8(f.readAll())}});
	}
	else if (t == proto::msg::Joined) { handleJoined(message); }
	else if (t == proto::msg::Tx)
	{
		m_txQueue.append(message);
		processTxQueue();
	}
	else if (t == proto::msg::Error) { fail(message.value("message").toString()); }
}


void CollabSession::handleJoined(const QJsonObject& message)
{
	m_seq = message.value("seq").toInteger();
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
		m_loadingSnapshot = true;
		Engine::getSong()->loadProject(file);
		m_loadingSnapshot = false;
	}
	startTracking();
	setState(State::Live);
}


void CollabSession::startTracking()
{
	stopTracking();
	std::vector<Track*> tracks = Engine::getSong()->tracks();
	const auto& patternTracks = Engine::patternStore()->tracks();
	tracks.insert(tracks.end(), patternTracks.begin(), patternTracks.end());

	for (Track* track : tracks)
	{
		auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
		if (!instrumentTrack) { continue; }
		for (Clip* clip : instrumentTrack->getClips())
		{
			if (auto midiClip = dynamic_cast<MidiClip*>(clip))
			{
				m_baselines[midiClip->collabId()] = currentNotes(*midiClip);
				trackClip(midiClip);
			}
		}
		// A shared clip can come back as a new object, e.g. when undo restores a deleted clip
		m_trackConnections.append(connect(instrumentTrack, &Track::clipAdded, this, [this](Clip* clip) {
			// Its saved id is only restored after creation, so look at it once the current event is done
			QTimer::singleShot(0, this, [this, clip = QPointer<Clip>{clip}] {
				auto midiClip = dynamic_cast<MidiClip*>(clip.data());
				if (midiClip && m_state == State::Live && m_baselines.contains(midiClip->collabId())
					&& m_trackers.find(midiClip) == m_trackers.end())
				{
					trackClip(midiClip);
					onClipChanged(midiClip);
				}
			});
		}));
	}

	ProjectJournal::setHook(this);

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
	ProjectJournal::setHook(nullptr);
	for (const auto& c : m_trackConnections) { disconnect(c); }
	m_trackConnections.clear();
	m_trackers.clear();
}


void CollabSession::trackClip(MidiClip* clip)
{
	auto tracker = std::make_unique<ClipTracker>();
	tracker->timer.setSingleShot(true);
	connect(&tracker->timer, &QTimer::timeout, this, [this, clip] { flushClip(clip); });
	tracker->changed = connect(clip, &MidiClip::dataChanged, this, [this, clip] { onClipChanged(clip); });
	tracker->destroyed = connect(clip, &MidiClip::destroyedMidiClip, this, [this](MidiClip* c) {
		m_trackers.erase(c);
	});
	m_trackers[clip] = std::move(tracker);
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
	const QString clipIdStr = proto::idString(clipId);
	NoteMap& baseline = m_baselines[clipId];
	const NoteMap current = currentNotes(*clip);
	const qint64 ctx = m_nextCtx;
	QJsonArray ops;

	// Remember this user's changes for undo, in the gesture that is open for this clip
	Gesture* gesture = nullptr;
	if (const auto open = m_openGesture.constFind(clipId); m_recordGestures && open != m_openGesture.cend())
	{
		if (const auto g = m_gestures.find(*open); g != m_gestures.end()) { gesture = &g->second; }
	}
	auto record = [gesture](collab_id_t id, const std::optional<NoteState>& before,
		const std::optional<NoteState>& after) {
		if (!gesture) { return; }
		if (!gesture->before.contains(id)) { gesture->before.insert(id, before); }
		gesture->after.insert(id, after);
	};

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
		record(it.key(), base == baseline.cend() ? std::nullopt : std::optional{*base}, *it);
		ops.append(QJsonObject{{"op", base == baseline.cend() ? proto::op::NoteAdd : proto::op::NoteSet},
			{"clip", clipIdStr}, {"id", proto::idString(it.key())}, {"v", values.toJson()}});
	}
	for (auto it = baseline.cbegin(); it != baseline.cend(); ++it)
	{
		if (current.contains(it.key())) { continue; }
		record(it.key(), *it, std::nullopt);
		m_pending.remove({clipId, it.key()});
		ops.append(QJsonObject{{"op", proto::op::NoteRemove}, {"clip", clipIdStr}, {"id", proto::idString(it.key())}});
	}

	baseline = current;
	if (!ops.isEmpty())
	{
		send({{"t", proto::msg::Tx}, {"ctx", ctx}, {"ops", ops}});
		++m_nextCtx;
	}
}


void CollabSession::processTxQueue()
{
	if (m_state != State::Live && m_state != State::Joining) { return; }
	const auto gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
	if (gui && QGuiApplication::mouseButtons() != Qt::NoButton)
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
		for (auto it = m_pending.begin(); it != m_pending.end();)
		{
			for (auto& fieldCtx : it.value())
			{
				if (fieldCtx <= ctx) { fieldCtx = 0; }
			}
			it = it.value() == PendingFields{} ? m_pending.erase(it) : std::next(it);
		}
		return;
	}

	// Group consecutive ops of the same clip so each clip is updated (and re-sorted) once
	const QJsonArray ops = message.value("ops").toArray();
	QJsonArray group;
	collab_id_t groupClip = 0;
	for (const QJsonValue& op : ops)
	{
		const collab_id_t clipId = proto::parseId(op.toObject().value("clip"));
		if (clipId != groupClip && !group.isEmpty())
		{
			applyRemoteOps(groupClip, group);
			group = QJsonArray{};
		}
		groupClip = clipId;
		group.append(op);
	}
	if (!group.isEmpty()) { applyRemoteOps(groupClip, group); }
}


std::uint64_t CollabSession::checkPointAdded(JournallingObject* jo)
{
	auto clip = dynamic_cast<MidiClip*>(jo);
	if (m_state != State::Live || !clip || !m_baselines.contains(clip->collabId())) { return 0; }

	// Changes made before this checkpoint belong to the previous gesture
	flushClip(clip);
	const std::uint64_t token = m_nextGesture++;
	m_gestures[token] = Gesture{clip->collabId(), {}, {}};
	m_openGesture[clip->collabId()] = token;
	while (m_gestures.size() > 1000) { m_gestures.erase(m_gestures.begin()); } // journal keeps 100 anyway
	return token;
}


void CollabSession::beforeRestore(JournallingObject*)
{
	if (m_state != State::Live) { return; }
	// Make sure every local change is synchronized (and recorded) before the journal restores anything
	for (const auto& [clip, tracker] : m_trackers) { flushClip(clip); }
}


void CollabSession::afterRestore(JournallingObject*, std::uint64_t token, bool undo)
{
	if (m_state != State::Live) { return; }
	// The journal restored an old snapshot; its notes may predate other users' changes
	resyncSharedClips();
	if (const auto g = m_gestures.find(token); token != 0 && g != m_gestures.end())
	{
		m_openGesture.remove(g->second.clip);
		replayGesture(g->second, undo);
	}
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


void CollabSession::resyncSharedClips()
{
	m_applyingRemote = true; // what we write here is the synchronized state: nothing to send
	for (auto it = m_baselines.cbegin(); it != m_baselines.cend(); ++it)
	{
		MidiClip* clip = findClip(it.key());
		if (!clip) { continue; }
		const NoteMap current = currentNotes(*clip);
		if (current == it.value()) { continue; }
		{
			auto guard = Engine::audioEngine()->requestChangesGuard();
			for (auto c = current.cbegin(); c != current.cend(); ++c)
			{
				if (!it.value().contains(c.key())) { writeNote(clip, c.key(), std::nullopt); }
			}
			for (auto b = it.value().cbegin(); b != it.value().cend(); ++b)
			{
				if (current.value(b.key()) != b.value() || !current.contains(b.key()))
				{
					writeNote(clip, b.key(), b.value());
				}
			}
			clip->rearrangeAllNotes();
			clip->updateLength();
		}
		emit clip->dataChanged();
	}
	m_applyingRemote = false;
}


void CollabSession::replayGesture(const Gesture& gesture, bool undo)
{
	MidiClip* clip = findClip(gesture.clip);
	if (!clip) { return; }

	// "mine" is what this user left (or, for redo, what they had before undoing); "target" what to go back to
	const auto& mineStates = undo ? gesture.after : gesture.before;
	const auto& targetStates = undo ? gesture.before : gesture.after;
	bool changed = false;
	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		for (auto it = mineStates.cbegin(); it != mineStates.cend(); ++it)
		{
			const collab_id_t id = it.key();
			const std::optional<NoteState>& mine = it.value();
			const std::optional<NoteState> target = targetStates.value(id);
			if (mine == target) { continue; }

			const Note* note = clip->findNote(id);
			const std::optional<NoteState> current = note ? std::optional{NoteState::of(*note)} : std::nullopt;

			if (!mine)
			{
				// This user removed the note: bring it back, unless it exists again by now
				if (!current) { writeNote(clip, id, target); changed = true; }
			}
			else if (!target)
			{
				// This user created the note: remove it only if nobody changed it since
				if (current == mine) { writeNote(clip, id, std::nullopt); changed = true; }
			}
			else if (current)
			{
				// Revert each field this user changed, unless someone changed that field afterwards
				NoteState next = *current;
				bool any = false;
				for (int f = 0; f < NoteValues::FieldCount; ++f)
				{
					if (mine->fields[f] != target->fields[f] && current->fields[f] == mine->fields[f])
					{
						next.fields[f] = target->fields[f];
						any = true;
					}
				}
				if (any) { writeNote(clip, id, next); changed = true; }
			}
			// else: someone else removed the note meanwhile; leave it removed
		}
		if (changed)
		{
			clip->rearrangeAllNotes();
			clip->updateLength();
		}
	}
	if (changed) { emit clip->dataChanged(); }

	// Send the result right away; it is the undo itself, not a new gesture to record
	m_recordGestures = false;
	flushClip(clip);
	m_recordGestures = true;
}


MidiClip* CollabSession::findClip(collab_id_t clipId)
{
	const auto owner = static_cast<const Clip*>(findOwner(IdScope::Clip, clipId));
	return dynamic_cast<MidiClip*>(const_cast<Clip*>(owner));
}


void CollabSession::applyRemoteOps(collab_id_t clipId, const QJsonArray& ops)
{
	if (!m_baselines.contains(clipId)) { return; } // not a shared clip
	MidiClip* clip = findClip(clipId);
	if (clip && m_trackers.find(clip) == m_trackers.end()) { clip = nullptr; }

	// Send our own unsent edits of this clip first, so the baseline only holds synchronized state
	if (clip) { flushClip(clip); }

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
				if (clip)
				{
					Note n{TimePos{*(*values)[NoteValues::Len]}, TimePos{*(*values)[NoteValues::Pos]},
						*(*values)[NoteValues::Key], static_cast<volume_t>(*(*values)[NoteValues::Vol]),
						static_cast<panning_t>(*(*values)[NoteValues::Pan])};
					n.setType(static_cast<Note::Type>(*(*values)[NoteValues::Type]));
					note = clip->addNote(n, false, noteId);
					changed = true;
				}
				else
				{
					NoteState s;
					for (int f = 0; f < NoteValues::FieldCount; ++f) { s.fields[f] = *values->fields[f]; }
					baseline.insert(noteId, s);
				}
			}
			else if (type == proto::op::NoteAdd || type == proto::op::NoteSet)
			{
				for (int f = 0; f < NoteValues::FieldCount; ++f)
				{
					const auto& v = values->fields[f];
					if (!v || pending[f] != 0) { continue; } // our unacknowledged write wins
					if (note)
					{
						switch (f)
						{
						case NoteValues::Key: note->setKey(*v); break;
						case NoteValues::Pos: note->setPos(TimePos{*v}); break;
						case NoteValues::Len: note->setLength(TimePos{*v}); break;
						case NoteValues::Vol: note->setVolume(static_cast<volume_t>(*v)); break;
						case NoteValues::Pan: note->setPanning(static_cast<panning_t>(*v)); break;
						case NoteValues::Type: note->setType(static_cast<Note::Type>(*v)); break;
						}
						changed = true;
					}
					else if (baseline.contains(noteId))
					{
						baseline[noteId].fields[f] = *v;
					}
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

} // namespace lmms::collab
