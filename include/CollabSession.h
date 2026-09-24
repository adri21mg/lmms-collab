/*
 * CollabSession.h - client side of a collaboration session
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

#ifndef LMMS_COLLAB_SESSION_H
#define LMMS_COLLAB_SESSION_H

#include <array>
#include <map>
#include <memory>
#include <optional>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include "CollabId.h"
#include "CollabProtocol.h"
#include "ProjectJournal.h"
#include "lmms_export.h"

class QTcpSocket;
class QTimer;

namespace lmms
{

class Clip;
class MidiClip;
class Note;
class Track;

namespace collab
{

//! Shared content of a note, i.e. what is synchronized (selection, playing state etc. stay local)
struct NoteState
{
	std::array<int, proto::NoteValues::FieldCount> fields{};

	static NoteState of(const Note& note);
	proto::NoteValues toValues() const;
	QJsonObject toJson() const { return toValues().toJson(); }
	static std::optional<NoteState> fromJson(const QJsonObject& obj);
	friend bool operator==(const NoteState&, const NoteState&) = default;
};

/**
 * Connection to a collaboration server and synchronization of the shared project.
 *
 * Local edits are detected at model level and compared with the last synchronized state ("baseline"):
 *  - notes: whenever a shared MidiClip reports dataChanged() (throttled to ~20 Hz)
 *  - structure (Song Editor tracks and clips): by comparing the model with the baseline ~20 times per
 *    second while connected, which catches every way of editing (drag, paste, menus, undo...)
 * The differences are sent as operations (see CollabProtocol.h). This needs no changes in the editors.
 *
 * Remote operations are applied to the model and written into the baseline at the same time, so they are
 * never detected as local changes and never echoed back. Transport (play/stop, playhead, loops, solo) is
 * never part of the session: every client renders the song with its own audio engine.
 *
 * Undo/redo (decision D4): each user only undoes their own changes. Every change is recorded per gesture
 * (the journal checkpoint an editor adds when an edit starts) as before/after states of the touched notes,
 * clips and tracks. Undoing a shared object reverts only this user's gesture, field by field, where the
 * value is still the one this user left; objects added or changed by others afterwards are never touched.
 */
class LMMS_EXPORT CollabSession : public QObject, public JournalHook
{
	Q_OBJECT
public:
	enum class State
	{
		Disconnected,
		Connecting,
		Joining,
		Live
	};

	enum class JoinMode
	{
		Create, //!< share the current song as a new project
		Open    //!< replace the current song with an existing shared project
	};

	static CollabSession* instance();

	void connectToServer(const QString& host, quint16 port, const QString& user, const QString& project,
		JoinMode mode);
	void disconnectFromServer();

	State state() const { return m_state; }
	QString projectName() const { return m_project; }
	QString userName() const { return m_user; }
	bool isApplyingRemote() const { return m_applyingRemote; }

	//! Sends local changes right away instead of at the next throttle tick (used by tests)
	void flushAll();

signals:
	void stateChanged();
	void errorOccurred(const QString& message);

public:
	// JournalHook
	std::uint64_t checkPointAdded(JournallingObject* jo) override;
	bool restore(JournallingObject* jo, std::uint64_t token, bool undo) override;
	void restored(JournallingObject* jo) override;

private:
	struct ClipTracker;
	using NoteMap = QHash<collab_id_t, NoteState>;
	using PendingFields = std::array<qint64, proto::NoteValues::FieldCount>; // ctx per field, 0 = none

	//! Synchronized structure of the Song Editor
	struct Structure
	{
		struct ClipInfo
		{
			collab_id_t track = 0;
			QJsonObject fields;
		};
		QList<collab_id_t> order;                 //!< shared tracks that take part in ordering
		QHash<collab_id_t, QJsonObject> tracks;   //!< fields of every shared track
		QHash<collab_id_t, int> trackTypes;
		QHash<collab_id_t, ClipInfo> clips;       //!< every shared Song Editor clip
	};

	enum class Kind { Track, Clip, Note };
	struct ObjectKey
	{
		Kind kind;
		collab_id_t parent; //!< clip of a note, track of a clip, 0 for tracks
		collab_id_t id;
		friend bool operator==(const ObjectKey&, const ObjectKey&) = default;
		friend size_t qHash(const ObjectKey& k, size_t seed = 0)
		{
			return qHashMulti(seed, static_cast<int>(k.kind), k.parent, k.id);
		}
	};
	using ObjectState = std::optional<QJsonObject>; //!< nullopt: the object does not exist

	//! This user's changes during one edit gesture
	struct Gesture
	{
		QHash<ObjectKey, ObjectState> before;
		QHash<ObjectKey, ObjectState> after;
		QHash<collab_id_t, QString> clipXml; //!< to recreate clips this gesture removed (or undo removed)
	};

	CollabSession();
	~CollabSession() override;

	void setState(State state);
	void fail(const QString& message);
	void send(const QJsonObject& message);
	void sendOps(const QJsonArray& ops);
	void onReadyRead();
	void handleMessage(const QJsonObject& message);
	void handleJoined(const QJsonObject& message);
	//! Transactions are applied strictly in server order, but only while no mouse button is held: an
	//! editor in the middle of a drag keeps pointers to the objects it edits, so they must not change
	//! (or disappear) under it. Queued transactions are applied as soon as the gesture ends.
	void processTxQueue();
	void applyTx(const QJsonObject& message);

	void startTracking();
	void stopTracking();

	// Notes
	void trackClip(MidiClip* clip);
	void untrackClip(collab_id_t clipId);
	void onClipChanged(MidiClip* clip);
	//! Compares the clip's notes with their baseline and sends the differences
	void flushClip(MidiClip* clip);
	void applyRemoteNoteOps(collab_id_t clipId, const QJsonArray& ops);
	//! Makes the note @p id of @p clip equal to @p target (nullopt removes it); does not re-sort the clip
	static void writeNote(MidiClip* clip, collab_id_t id, const std::optional<NoteState>& target);
	static NoteMap currentNotes(const MidiClip& clip);

	// Structure
	//! Current Song Editor structure: tracks already shared, plus new tracks of types that can be shared
	Structure currentStructure() const;
	void flushStructure();
	void applyRemoteStructureOp(const QJsonObject& op);
	//! Registers newly shared clips for note synchronization and forgets removed ones
	void syncNoteTracking();
	static QJsonObject trackFields(const Track* track);
	static QJsonObject clipFields(const Clip* clip);
	static void applyTrackFields(Track* track, const QJsonObject& fields);
	static void applyClipFields(Clip* clip, const QJsonObject& fields);
	//! Complete XML of a track or clip as sent to others (private window state removed)
	static QString serialize(Track* track);
	static QString serialize(Clip* clip);
	static Clip* createClipFromXml(Track* track, const QString& xml);
	static void removeTrack(Track* track);
	static void removeClip(Clip* clip);
	static void reorderTracks(const QList<collab_id_t>& order);
	static bool syncsStructure(int trackType);
	static bool takesPartInOrder(int trackType);

	// Undo
	Gesture* openGestureFor(Kind kind, collab_id_t parent, collab_id_t id);
	void record(Kind kind, collab_id_t parent, collab_id_t id, const ObjectState& before, const ObjectState& after);
	//! Reverts (undo) or re-applies (redo) this user's changes of a gesture where nobody changed them since
	void replayGesture(Gesture& gesture, bool undo);
	ObjectState currentState(const ObjectKey& key) const;
	bool isShared(JournallingObject* jo) const;
	//! Notes of a clip as one comparable value, so undo never removes a clip whose content changed
	static QString notesSignature(const NoteMap& notes);

	static MidiClip* findMidiClip(collab_id_t clipId);
	static Clip* findClip(collab_id_t clipId);
	static Track* findTrack(collab_id_t trackId);

	std::unique_ptr<QTcpSocket> m_socket;
	proto::FrameDecoder m_decoder;
	State m_state = State::Disconnected;
	JoinMode m_joinMode = JoinMode::Open;
	QString m_user;
	QString m_project;
	QString m_clientId;
	qint64 m_seq = 0;
	qint64 m_nextCtx = 1;
	bool m_applyingRemote = false;
	bool m_loadingSnapshot = false;

	//! Last synchronized notes per shared MIDI clip (Song Editor and Pattern Editor)
	QHash<collab_id_t, NoteMap> m_baselines;
	//! Last synchronized Song Editor structure
	Structure m_structure;
	//! Local writes not yet acknowledged by the server. Remote values for these fields are ignored:
	//! our own write is ordered after them by the server and wins anyway.
	QHash<QPair<collab_id_t, collab_id_t>, PendingFields> m_pending;  //!< notes, per (clip, note)
	QHash<QString, qint64> m_pendingStructure;                        //!< "kind:id:field" -> ctx
	std::map<MidiClip*, std::unique_ptr<ClipTracker>> m_trackers;
	QList<QMetaObject::Connection> m_trackConnections;
	QList<QJsonObject> m_txQueue;
	QTimer* m_txQueueTimer;
	QTimer* m_structureTimer;

	std::map<std::uint64_t, Gesture> m_gestures;                      //!< by journal token
	QHash<QPair<int, collab_id_t>, std::uint64_t> m_openGesture;      //!< (kind, id) -> gesture collecting changes
	std::uint64_t m_nextGesture = 1;
	bool m_recordGestures = true;
};

} // namespace collab

} // namespace lmms

#endif // LMMS_COLLAB_SESSION_H
