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
#include <QObject>
#include <QSet>

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

namespace collab
{

//! Shared content of a note, i.e. what is synchronized (selection, playing state etc. stay local)
struct NoteState
{
	std::array<int, proto::NoteValues::FieldCount> fields{};

	static NoteState of(const Note& note);
	proto::NoteValues toValues() const;
	friend bool operator==(const NoteState&, const NoteState&) = default;
};

/**
 * Connection to a collaboration server and synchronization of the shared project.
 *
 * Local edits are detected at model level: whenever a shared MidiClip reports dataChanged(), its notes
 * are compared (throttled to ~20 Hz) with the last synchronized state ("baseline") and the differences are
 * sent as note.add / note.set / note.remove operations. This covers every way of editing notes (Piano
 * Roll tools, paste, step sequencer, recording, undo...) without touching the editors.
 *
 * Remote operations are applied to the model and written into the baseline at the same time, so they are
 * never detected as local changes and never echoed back. Transport (play/stop, playhead, loops) is never
 * part of the session: every client renders the song with its own audio engine.
 *
 * Undo/redo (decision D4): each user only undoes their own changes. Every note edit is recorded per gesture
 * (the journal checkpoint an editor adds when an edit starts) as before/after states of the touched notes.
 * LMMS still restores its snapshot for everything else, but notes of shared clips are then put back to the
 * shared state, and only this user's gesture is reverted, field by field, where the value is still the one
 * this user left. Notes added or changed by others afterwards are never touched.
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

signals:
	void stateChanged();
	void errorOccurred(const QString& message);

public:
	// JournalHook
	std::uint64_t checkPointAdded(JournallingObject* jo) override;
	void beforeRestore(JournallingObject* jo) override;
	void afterRestore(JournallingObject* jo, std::uint64_t token, bool undo) override;

private:
	struct ClipTracker;
	using NoteMap = QHash<collab_id_t, NoteState>;
	using PendingFields = std::array<qint64, proto::NoteValues::FieldCount>; // ctx per field, 0 = none

	//! This user's note changes during one edit gesture on one clip
	struct Gesture
	{
		collab_id_t clip = 0;
		QHash<collab_id_t, std::optional<NoteState>> before; //!< nullopt: the note did not exist
		QHash<collab_id_t, std::optional<NoteState>> after;  //!< nullopt: the note was removed
	};

	CollabSession();
	~CollabSession() override;

	void setState(State state);
	void fail(const QString& message);
	void send(const QJsonObject& message);
	void onReadyRead();
	void handleMessage(const QJsonObject& message);
	void handleJoined(const QJsonObject& message);
	//! Transactions are applied strictly in server order, but only while no mouse button is held: an
	//! editor in the middle of a drag keeps pointers to the notes it edits, so they must not change
	//! (or disappear) under it. Queued transactions are applied as soon as the gesture ends.
	void processTxQueue();
	void applyTx(const QJsonObject& message);

	void startTracking();
	void stopTracking();
	void trackClip(MidiClip* clip);
	void onClipChanged(MidiClip* clip);
	//! Compares the clip with its baseline and sends the differences
	void flushClip(MidiClip* clip);
	void applyRemoteOps(collab_id_t clipId, const QJsonArray& ops);
	//! Makes the note @p id of @p clip equal to @p target (nullopt removes it); does not re-sort the clip
	static void writeNote(MidiClip* clip, collab_id_t id, const std::optional<NoteState>& target);
	//! Puts the notes of every shared clip back to the synchronized state, without sending anything
	void resyncSharedClips();
	//! Reverts (undo) or re-applies (redo) this user's changes of a gesture where nobody changed them since
	void replayGesture(const Gesture& gesture, bool undo);
	static NoteMap currentNotes(const MidiClip& clip);
	static MidiClip* findClip(collab_id_t clipId);

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

	//! Last synchronized notes per shared clip id. Kept even while a clip object does not exist (e.g. a
	//! clip deleted locally and brought back by undo is compared against it again).
	QHash<collab_id_t, NoteMap> m_baselines;
	//! Local writes not yet acknowledged by the server, per (clip, note). Remote values for these fields
	//! are ignored: our own write is ordered after them by the server and wins anyway.
	QHash<QPair<collab_id_t, collab_id_t>, PendingFields> m_pending;
	std::map<MidiClip*, std::unique_ptr<ClipTracker>> m_trackers;
	QList<QMetaObject::Connection> m_trackConnections;
	QList<QJsonObject> m_txQueue;
	QTimer* m_txQueueTimer;

	std::map<std::uint64_t, Gesture> m_gestures;   //!< by journal token
	QHash<collab_id_t, std::uint64_t> m_openGesture; //!< gesture currently collecting a clip's changes
	std::uint64_t m_nextGesture = 1;
	bool m_recordGestures = true;
};

} // namespace collab

} // namespace lmms

#endif // LMMS_COLLAB_SESSION_H
