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
#include <QDomElement>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>

#include "AutomatableModel.h"
#include "CollabId.h"
#include "CollabProtocol.h"
#include "ProjectJournal.h"
#include "lmms_export.h"

class QTcpSocket;
class QTimer;

namespace lmms
{

class AutomatableModel;
class AutomationClip;
class Clip;
class Effect;
class EffectChain;
class MidiClip;
class MixerChannel;
class Note;
class Track;
class TrackContainer;

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
		JoinMode mode, const QString& color = {});

	//! Where this user is: {cursor, play, view}, see proto::sanitizePresence; ignored unless connected
	void sendPresence(const QJsonObject& presence);
	QString userColor() const { return m_color; }
	void disconnectFromServer();

	State state() const { return m_state; }
	QString projectName() const { return m_project; }
	QString userName() const { return m_user; }
	bool isApplyingRemote() const { return m_applyingRemote; }

	//! Sends local changes right away instead of at the next throttle tick (used by tests)
	void flushAll();

	//! Diagnostics: "path fingerprint name" of every synchronized parameter of a track
	static QStringList describeParams(Track* track);
	//! Diagnostics: appends a line to <workspace>/collab/session.log (recreated on every connection)
	void log(const QString& line);

signals:
	void stateChanged();
	void errorOccurred(const QString& message);
	//! Another user's presence changed ({clientId, user, color, cursor, play} or {clientId, gone})
	void presenceReceived(const QJsonObject& presence);
	//! The session ended: forget everybody's presence
	void presenceCleared();

public:
	// JournalHook
	std::uint64_t checkPointAdded(JournallingObject* jo) override;
	bool restore(JournallingObject* jo, std::uint64_t token, bool undo) override;
	void restored(JournallingObject* jo) override;

private:
	struct ClipTracker;
	using NoteMap = QHash<collab_id_t, NoteState>;
	using PendingFields = std::array<qint64, proto::NoteValues::FieldCount>; // ctx per field, 0 = none

	//! Synchronized structure of the Song Editor and the Pattern Editor
	struct Structure
	{
		struct ClipInfo
		{
			collab_id_t track = 0;
			QJsonObject fields;
		};
		QList<collab_id_t> order;                 //!< Song Editor tracks in order (incl. pattern tracks)
		QList<collab_id_t> patternOrder;          //!< Pattern Editor tracks in order
		QHash<collab_id_t, QJsonObject> tracks;   //!< fields of every shared track
		QHash<collab_id_t, int> trackTypes;
		QSet<collab_id_t> patternEditorTracks;
		QHash<collab_id_t, ClipInfo> clips;       //!< every shared clip (Pattern Editor clips: no "pos")
		std::optional<QString> notes;             //!< project notes (only with a GUI)

		struct ChannelInfo
		{
			QJsonObject fields;                   //!< name, color, (shared) muted
			QList<collab_id_t> sends;             //!< channels this one sends to
		};
		QList<collab_id_t> channels;              //!< mixer channels in order, the master first
		QHash<collab_id_t, ChannelInfo> channelInfo;
	};

	enum class Kind { Track, Clip, Note, Param, Automation };
	struct ObjectKey
	{
		Kind kind;
		collab_id_t parent; //!< clip of a note or automation, track of a clip, owner of a parameter (0: song),
		                    //!< 0 for tracks
		collab_id_t id;
		QString path = {};  //!< parameter path (see proto::op::ParamSet)
		friend bool operator==(const ObjectKey&, const ObjectKey&) = default;
		friend size_t qHash(const ObjectKey& k, size_t seed = 0)
		{
			return qHashMulti(seed, static_cast<int>(k.kind), k.parent, k.id, k.path);
		}
	};

	//! A synchronized parameter: owner track (0 for the song) and its path
	using ParamKey = QPair<collab_id_t, QString>;
	//! Parameters of one track by path and the other way round (rebuilt regularly: plugins may add some)
	struct ParamIndex
	{
		std::map<QString, QPointer<AutomatableModel>> byPath;
		QHash<const AutomatableModel*, QString> byModel;
	};
	//! Who made the latest change to a track's parameters, to know who stores its settings on the server
	struct TrackParamState
	{
		qint64 lastSeq = 0;
		bool mine = false;
		bool dirty = false;
		QElapsedTimer lastChange;
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
	//! Moves tracks one step at a time, as dragging does, so pattern tracks take their patterns along
	static void reorderTracks(TrackContainer* container, const QList<collab_id_t>& order);
	static bool syncsStructure(int trackType, bool inPatternEditor);
	void addRemotePattern(const QJsonObject& op);
	//! Project notes currently shown, if there is a GUI
	static std::optional<QString> currentNotes();

	// Automation (milestone M5b)
	//! Nodes, curve and automated parameters of an automation clip (parameters as owner + path, see
	//! proto::op::AutomationSet), as sent in automation.set
	QJsonObject automationContent(const AutomationClip* clip) const;
	//! The same as one comparable string (a hidden structure field of automation clips)
	QString automationSignature(const AutomationClip* clip) const;
	void applyAutomationContent(AutomationClip* clip, const QJsonObject& content);
	//! Adds owner + path to the <object> elements of automation clips in @p root (LMMS saves journal ids,
	//! which are different on every client), and removes those ids
	static void annotateAutomation(QDomElement root);
	//! Connects the automation clips found in @p root to the parameters named by owner + path
	void resolveAutomation(const QDomElement& root);
	//! Sets the automated parameters of a clip; parameters that do not exist yet (e.g. of a track that
	//! arrives later) are tried again after the next transaction
	void setAutomationObjects(AutomationClip* clip, const QJsonArray& objects);
	void retryAutomationObjects();

	// Mixer (milestone M5a)
	//! Adds the mixer's channels, their fields and sends to @p s
	static void addMixerStructure(Structure& s);
	//! Appends the operations for mixer changes since the baseline (and a mixer.state if there are any)
	void flushMixer(const Structure& current, QJsonArray& ops, qint64 ctx);
	void applyRemoteMixerOp(const QJsonObject& op);
	//! The whole <mixer> as the server stores it (solo removed: it is private)
	static QString mixerXml();
	//! Parameters of a mixer channel: "c:0" volume, "s:<channel>" send amounts, "fx:<n>" its effects
	static std::vector<std::pair<QString, AutomatableModel*>> enumerateChannelParams(MixerChannel* channel);

	// Parameters (knobs, sliders, buttons, combo boxes of tracks, instruments, effects; song tempo etc.)
	//! Parameters of a track: "t:<n>" in its own object tree, "fx:<n>" in its effect chain
	static std::vector<std::pair<QString, AutomatableModel*>> enumerateParams(Track* track);
	static std::vector<std::pair<QString, AutomatableModel*>> songParamModels();
	void refreshParamIndex();
	//! Compares parameter values with their baseline and sends what the user changed by hand
	void flushParams();
	void applyRemoteParam(const QJsonObject& op, qint64 seq);
	//! Sends the settings of tracks whose knobs came to rest, if this client made their latest change
	void sendTrackStates();
	void paramsAcknowledged(qint64 ctx, qint64 seq);
	std::optional<ParamKey> paramKeyOf(const AutomatableModel* model) const;
	AutomatableModel* findParam(const ParamKey& key);
	//! Forgets the baselines of a track's parameters with a path prefix (e.g. after its instrument changed)
	void resetParamBaselines(collab_id_t owner, const QString& prefix);
	void noteParamActivity(collab_id_t owner, bool local);

	// Instruments and effect chains as a whole (milestone M4b)
	//! Sends instrument changes (other plugin, preset), effect chain changes (added, removed, moved effects)
	//! and changes of plugin state that is not made of parameters (samples, ZynAddSubFX, VST...)
	void flushPlugins();
	void applyRemoteInstrument(const QJsonObject& op);
	void applyRemoteEffects(const QJsonObject& op);
	//! Records the current instrument/effects of a track as synchronized
	void rebasePlugins(Track* track);
	void rebaseEffects(collab_id_t owner, EffectChain* chain);
	struct PluginState;
	//! Queues effects.set for the effects of a track or mixer channel ("track"/"channel": @p ownerKey) if
	//! they changed; returns true if so
	bool flushEffects(collab_id_t owner, const char* ownerKey, EffectChain* chain, PluginState& state,
		bool compareState, bool onlyRemoteKnobs);
	//! Tracks and mixer channels whose instrument or effect windows are open (their plugin state may change
	//! without knobs)
	static QSet<collab_id_t> tracksBeingEdited();

	// Undo
	Gesture* openGestureFor(Kind kind, collab_id_t parent, collab_id_t id, const QString& path = {});
	void record(Kind kind, collab_id_t parent, collab_id_t id, const ObjectState& before, const ObjectState& after,
		const QString& path = {});
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
	QString m_color;
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
	QElapsedTimer m_notesSent; //!< notes are sent at most twice per second

	QHash<collab_id_t, ParamIndex> m_paramIndex;       //!< per shared track
	QElapsedTimer m_paramIndexAge;
	QHash<ParamKey, float> m_paramBaseline;            //!< last synchronized value of every parameter
	QHash<ParamKey, qint64> m_pendingParams;           //!< our unacknowledged writes (ctx)
	QHash<collab_id_t, TrackParamState> m_trackParams;
	QHash<qint64, QSet<collab_id_t>> m_ctxParamTracks; //!< tracks whose parameters a local tx changed
	QTimer* m_trackStateTimer;
	QHash<ParamKey, std::uint64_t> m_openParamGesture;
	std::unique_ptr<QFile> m_log;

	//! Last synchronized instrument and effect chain of a track
	struct PluginState
	{
		QString instrumentIdentity; //!< which instrument object/plugin (changes when it is replaced)
		QString instrumentXml;
		QString effectsIdentity;    //!< which effects, in which order
		QString effectsXml;
		bool localParamsTouched = false;  //!< our knob changes since the XML above was taken
		bool remoteParamsTouched = false; //!< the other side's knob changes since then
		QElapsedTimer lastParamActivity;
		bool known = false;         //!< the state above was taken at least once
	};
	QHash<collab_id_t, PluginState> m_plugins;
	QJsonArray m_pluginOps; //!< effects.set of the current flushPlugins(), before the states that store them
	QHash<collab_id_t, QJsonArray> m_unresolvedAutomation; //!< automated parameters not found yet, per clip
	QSet<collab_id_t> m_editedTracks;  //!< tracks with open plugin windows at the last check
	QSet<collab_id_t> m_externalGuiTracks; //!< edited tracks whose plugins have a window of their own
	QElapsedTimer m_opaqueCheck;

	std::map<std::uint64_t, Gesture> m_gestures;                      //!< by journal token
	QHash<QPair<int, collab_id_t>, std::uint64_t> m_openGesture;      //!< (kind, id) -> gesture collecting changes
	std::uint64_t m_nextGesture = 1;
	bool m_recordGestures = true;
};

//! Id of the track or mixer channel whose effect chain holds @p effect, or 0
LMMS_EXPORT collab_id_t effectOwner(const Effect* effect);
//! Effect chain of a track or mixer channel, or nullptr
LMMS_EXPORT EffectChain* effectChainOf(collab_id_t owner);
//! Name of the track or mixer channel @p owner
LMMS_EXPORT QString effectOwnerName(collab_id_t owner);
//! Position of @p effect in its chain, or -1
LMMS_EXPORT int effectIndex(const Effect* effect);
LMMS_EXPORT Effect* effectAt(collab_id_t owner, int index);
//! The mixer channel with id @p id, or nullptr
LMMS_EXPORT MixerChannel* findMixerChannel(collab_id_t id);
//! Position of the mixer channel @p id, or -1
LMMS_EXPORT int mixerChannelIndex(collab_id_t id);

} // namespace collab

} // namespace lmms

#endif // LMMS_COLLAB_SESSION_H
