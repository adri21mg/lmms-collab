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
#include <QMap>
#include <QDateTime>
#include <QDomElement>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>

#include "AutomatableModel.h"
#include "CollabConnection.h"
#include "CollabId.h"
#include "CollabProtocol.h"
#include "ProjectJournal.h"
#include "lmms_export.h"

class QProgressDialog;
class QTcpSocket;
class QTimer;

namespace lmms
{

class AutomatableModel;
class AutomationClip;
class Clip;
class Controller;
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
		Live,
		Reconnecting //!< the connection was lost; trying again every few seconds
	};

	//! Whether this user's work is safe on the server
	enum class SyncStatus
	{
		Sending, //!< local changes not acknowledged by the server yet
		Saving,  //!< everything arrived; the server writes it to disk within a few seconds
		Saved    //!< on the server's disk
	};

	enum class JoinMode
	{
		Create, //!< share the current song as a new project
		Open    //!< replace the current song with an existing shared project
	};

	static CollabSession* instance();

	//! @p passwordKey: proto::passwordKey of the server's password, if it has one
	void connectToServer(const QString& host, quint16 port, const QString& user, const QString& project,
		JoinMode mode, const QString& color = {}, const QByteArray& passwordKey = {});
	//! The connection to the server is encrypted (TLS)
	bool isEncrypted() const { return m_encrypted; }
	//! SHA-256 of the server's certificate when encrypted
	QString serverFingerprint() const { return m_fingerprint; }

	//! Where this user is: {cursor, play, view}, see proto::sanitizePresence; ignored unless connected
	void sendPresence(const QJsonObject& presence);
	QString userColor() const { return m_color; }
	//! This user's color from now on (also for the others)
	void setColor(const QString& color);
	//! The server's name for this connection ("c12"); later connections have larger numbers
	QString clientId() const { return m_clientId; }
	void disconnectFromServer();
	//! Leaves the session for good (also stops reconnecting)
	void leave();
	//! Asks the server to save and waits up to @p timeoutMs until it did; false if it did not (or not connected)
	bool waitUntilSaved(int timeoutMs);
	//! Whether the last lost connection came back with changes made offline
	bool hadOfflineChanges() const { return m_hadOfflineChanges; }
	//! The connection was lost and the session is trying to get it back (through every attempt's states)
	bool isReconnecting() const { return m_reconnecting; }

	State state() const { return m_state; }
	QString projectName() const { return m_project; }
	QString userName() const { return m_user; }
	bool isApplyingRemote() const { return m_applyingRemote; }
	SyncStatus syncStatus() const;
	//! When the server last wrote the project to disk (invalid if not yet in this session)
	QDateTime lastSaved() const { return m_savedAt; }
	//! Asks the server to write the project to disk now
	void saveNow();
	//! Asks the server for a version of the project as it is now (everything sent before is in it)
	void createVersion(const QString& description);
	//! Asks the server for the project's versions (answered by versionsReceived)
	void requestVersions();
	//! Asks the server to make version @p id the project again, for everyone (the state before becomes a version)
	void restoreVersion(int id);

	//! Sends local changes right away instead of at the next throttle tick (used by tests)
	void flushAll();

	//! Diagnostics: "path fingerprint name" of every synchronized parameter of a track
	static QStringList describeParams(Track* track);
	//! Diagnostics: appends a line to <workspace>/collab/session.log (recreated on every connection)
	void log(const QString& line);

signals:
	void stateChanged();
	void syncStatusChanged();
	void errorOccurred(const QString& message);
	//! Another user's presence changed ({clientId, user, color, cursor, play} or {clientId, gone})
	void presenceReceived(const QJsonObject& presence);
	//! The session ended: forget everybody's presence
	void presenceCleared();
	//! Someone (maybe this user) created a version: {id, description, by, at, seq, p4}
	void versionCreated(const QJsonObject& version);
	//! The server's Perforce submit of a version ended: p4 = {state, change?, error?}
	void versionPerforce(int id, const QJsonObject& p4);
	void versionsReceived(const QJsonArray& versions);
	//! Plugins this project uses that are not installed here (each one told once per session)
	void missingPlugins(const QStringList& names);
	//! The server's certificate is not the one remembered for it (the connection was refused)
	void serverCertificateChanged(const QString& host, quint16 port, const QString& fingerprint);
	//! A version request could not be done (the session goes on)
	void versionError(const QString& message);
	//! Someone restored a version: {id, by, description, safety} (the project is being loaded again)
	void versionRestored(const QJsonObject& restore);

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
		QList<collab_id_t> controllers;           //!< LFO controllers of the Controller Rack, in order
		QHash<collab_id_t, QJsonObject> controllerFields;
		QStringList allControllers;               //!< every shared controller (also Peak Controllers), in order
		QHash<QString, QString> peakNames;        //!< names of Peak Controllers (see controllerName())
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
	//! A plugin the project uses is not installed here: the user is told (once, together with others)
	void notePluginMissing(const QString& name, const QString& reason);
	//! Forgets what is known about the project (baselines, pending changes); the connection stays
	void forgetProjectState();
	//! A version was restored: the project is loaded again from what the server sent
	void reloadRestored(const QJsonObject& message);
	//! Joining: removes clips the shared project @p mmp does not have (made by LMMS while it was loading)
	void removeClipsNotIn(const QString& mmp);
	//! Ops as one log line (without their XML, with the id of what it holds)
	static QString opsSummary(const QJsonArray& ops);
	//! The server did not take some of our changes: load the project again from it (offering this version as a file)
	void resyncAfterRejection();
	void onReadyRead();
	void handleMessage(const QJsonObject& message);
	void handleJoined(const QJsonObject& message);
	//! The server welcomed us (encryption and password done): the session starts
	void onConnected(const QJsonObject& welcome);
	//! The connection was lost in a session: try again until the user leaves
	void startReconnecting();
	//! Back after a lost connection: what this user changed meanwhile is offered as a file of its own (D5)
	bool keepOfflineChanges();
	//! The shared content of the song (no window positions), to compare it before and after a lost connection
	static QString projectSnapshot();
	//! Loads the joined project (after its shared files are here) and goes live
	void finishJoin(const QJsonObject& message);
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

	// Shared files (milestone M6a)
	struct SharedFile
	{
		QString path;  //!< inside libraryDir(); "shared:<path>" in the project
		qint64 size = 0;
	};
	//! This project's shared files on this computer (<workspace>/collab/<project>/library)
	QString libraryDir() const;
	void setLibrary(const QJsonArray& files);
	//! A shared file announced by the server or a collaborator (@p by); downloaded if it is not here
	void addLibraryFile(const QString& path, const QString& hash, qint64 size, bool fromOthers, const QString& by = {});
	//! The "Shared project" tab shows this project's files (or only LMMS' own when not connected)
	void refreshSharedFiles();
	//! Shared files of the project missing here
	QStringList missingFiles() const;
	//! Before joining a project: downloads its shared files missing here. With a GUI the user agrees first
	//! (or does not join: a project without its sounds makes no sense) and sees the progress. Returns false if
	//! the user does not want to join.
	bool downloadMissing(const QStringList& missing);
	void updateDownloadProgress();
	//! Called when a download ended: the join waiting for it continues once all are here
	void downloadsChanged();
	void requestNextDownload();
	void handleAssetMessage(const QJsonObject& message);
	void handleBinary(const QByteArray& payload);
	//! Files of this computer (not LMMS' own, not shared) that @p xml refers to: value as saved -> absolute path
	static QHash<QString, QString> localFileRefs(const QString& xml);
	//! Whether an object with this XML can be sent: files only this computer has are shared first (asking
	//! the user); until then the object waits
	bool readyToSend(const QString& xml);
	//! Shares the local files waiting in m_shareQueue: known ones right away, new ones if the user agrees
	void processShareQueue();
	void uploadFile(const QString& absolutePath);
	//! Makes every instrument and sample clip that uses @p absoluteLocal use the shared file instead
	void rewriteReferences(const QString& absoluteLocal, const QString& sharedPath);
	//! The user did not share @p absoluteLocal: what uses it is undone (new objects removed)
	void dropReferences(const QString& absoluteLocal);
	//! A shared file arrived: whatever uses it loads it again
	void reloadReferences(const QString& sharedPath);
	//! After creating a project: the song's own files that only this computer has
	void shareProjectFiles();

	// Controllers (milestone M5c)
	static void addControllerStructure(Structure& s);
	//! Appends the operations for Controller Rack changes (and a controllers.state if there are any)
	void flushControllers(const Structure& current, QJsonArray& ops, qint64 ctx);
	void applyRemoteControllerOp(const QJsonObject& op);
	//! The whole <controllers>, as LMMS saves it
	static QString controllersXml();
	//! Parameters of a controller: "k:<n>", the n-th knob or button in its object tree
	static std::vector<std::pair<QString, AutomatableModel*>> enumerateControllerParams(Controller* controller);
	//! The shared controller connection of a parameter: a controller name (see proto::op::ParamLink) or ""
	//! (none, or a MIDI controller: each user's own device)
	static QString sharedConnection(const AutomatableModel* model);
	void applyRemoteLink(const QJsonObject& op, qint64 seq);
	//! Connects @p model to the controller named @p controller ("" disconnects); false if it does not exist here
	static bool applyLink(AutomatableModel* model, const QString& controller);
	//! Connections to controllers that did not exist yet (e.g. a Peak Controller effect that arrives later)
	void retryLinks();

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
	std::unique_ptr<CollabConnection> m_connection; //!< until the server welcomed us
	QByteArray m_passwordKey;
	bool m_encrypted = false;
	QString m_fingerprint;
	proto::FrameDecoder m_decoder;
	State m_state = State::Disconnected;
	JoinMode m_joinMode = JoinMode::Open;
	QString m_user;
	QString m_color;
	QString m_project;
	QString m_clientId;
	qint64 m_seq = 0;
	qint64 m_nextCtx = 1;
	qint64 m_ackedCtx = 0;   //!< our latest transaction the server acknowledged
	qint64 m_savedSeq = 0;   //!< the server's disk holds the project up to this transaction
	QDateTime m_savedAt;
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
	QHash<ParamKey, QString> m_linkBaseline;     //!< last synchronized controller connection of every parameter
	QHash<ParamKey, qint64> m_pendingLinks;      //!< our unacknowledged connection changes (ctx)
	QHash<ParamKey, QString> m_unresolvedLinks;  //!< connections to controllers not found yet

	QHash<QString, SharedFile> m_library;        //!< shared files of the project by SHA-256
	QHash<QString, QString> m_localHashes;       //!< SHA-256 of local files already hashed (absolute path)
	QStringList m_shareQueue;                    //!< local files to share (absolute paths)
	QSet<QString> m_askingAbout;                 //!< local files the user is being asked about right now
	QHash<QString, QString> m_uploads;           //!< uploads waiting for asset.stored: hash -> absolute path
	bool m_shareScheduled = false;
	bool m_asking = false;
	QStringList m_downloadQueue;                 //!< hashes
	QString m_downloading;                       //!< hash being downloaded
	qint64 m_downloadSize = 0;
	qint64 m_downloadReceived = 0;
	std::unique_ptr<QFile> m_downloadFile;
	QPointer<QProgressDialog> m_downloadProgress; //!< while joining
	qint64 m_downloadTotal = 0;                  //!< bytes the progress dialog shows
	qint64 m_downloadDone = 0;
	std::optional<QJsonObject> m_pendingJoin;    //!< joined message waiting for its shared files
	bool m_reading = false;                      //!< in onReadyRead() (dialogs process events meanwhile)
	QString m_host;
	quint16 m_port = 0;
	bool m_reconnecting = false;                 //!< the session was lost; connections are attempts to get it back
	int m_reconnectAttempts = 0;
	QString m_offlineSnapshot;                   //!< the project when the connection was lost (to see what changed)
	bool m_unsentAtLoss = false;
	bool m_hadOfflineChanges = false;
	bool m_rejectedChanges = false;
	QSet<QString> m_missingPlugins;              //!< told to the user in this session
	QStringList m_newMissingPlugins;             //!< to tell (together, a moment later)              //!< reloading because the server did not take some of our changes
	QMap<qint64, QJsonArray> m_unacknowledgedOps; //!< by ctx, until the server acknowledged them                 //!< own changes had not reached the server when it was lost
	bool m_sharedFilesRefresh = false;           //!< a refresh of the "Shared project" tab is scheduled
	QSet<collab_id_t> m_forcePluginCheck;        //!< instruments whose state must be compared at the next flush
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
//! How collaborators name a controller: its id (LFO), "p:<owner>:<n>" (Peak Controller effect) or "" (MIDI)
LMMS_EXPORT QString controllerName(Controller* controller);
//! The controller named @p name (see controllerName()), or nullptr
LMMS_EXPORT Controller* findController(const QString& name);
//! Position of the mixer channel @p id, or -1
LMMS_EXPORT int mixerChannelIndex(collab_id_t id);

} // namespace collab

} // namespace lmms

#endif // LMMS_COLLAB_SESSION_H
