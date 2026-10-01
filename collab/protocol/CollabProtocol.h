/*
 * CollabProtocol.h - wire protocol shared by the LMMS collaboration client and server
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

// Transport: TCP. Every frame is  [uint32 big-endian payload length][uint8 frame type][payload].
// Frame type 0 carries one UTF-8 JSON object (a message), type 1 is reserved for binary data (assets).
//
// Messages ("t" field):
//   client -> server
//     hello   {proto, user}                          first message after connecting
//     create  {project, mmp}                         create a shared project from the client's song
//     open    {project}                              join an existing shared project
//     tx      {ctx, ops[]}                           one local edit gesture (client-local counter ctx)
//     presence {cursor, play}                        where this user is; never stored (see sanitizePresence)
//     asset.put {hash, size, name}                   upload of a shared file (M6a): binary frames follow
//     asset.get {hash}                               download of a shared file
//   server -> client
//     welcome {proto, clientId}
//     joined  {project, seq, mmp?}                   mmp is omitted for the creator (it already has the song)
//     tx      {seq, clientId, ctx, ops[]}            accepted transaction, sent to every client incl. sender
//     presence {clientId, user, color, cursor, play} another user's presence, or {clientId, gone:true}
//     asset.stored {hash, path}                      an upload is complete (or the file was already there)
//     asset.data {hash, size}                        a download: binary frames follow
//     asset.error {hash, message}
//     error   {message}
// hello also carries the user's color ("#rrggbb"). joined also carries the shared files: library [{path, hash, size}].
// Binary frames (type 1) carry file data: [32 bytes SHA-256 of the whole file][up to AssetChunkSize bytes].
// Shared files are named "shared:<path>" in the project, like LMMS' own "factorysample:..." paths; every client
// keeps them in its own folder. Files are identified by their SHA-256 (the same file is stored once).
//
// Ops inside a tx (field names match LMMS' XML attributes):
//     {op:"note.add",    clip, id, v:{key,pos,len,vol,pan,type}}   all fields required
//     {op:"note.set",    clip, id, v:{any subset}}                 absolute values, never deltas
//     {op:"note.remove", clip, id}
//     {op:"track.add",   container:"song", index, xml}             complete <track> element (with its clips)
//     {op:"track.remove", id}
//     {op:"track.set",   id, v:{name, muted, color, channel}}      any subset; color "" = no color;
//                                                                   channel = id of its mixer channel
//     {op:"track.order", container:"song", ids:[...]}              order of the shared tracks
//     {op:"clip.add",    track, xml}                               complete clip element (with its notes)
//     {op:"clip.remove", id}
//     {op:"clip.set",    id, v:{pos,len,off,name,color,muted,autoresize,steps,src}}  any subset
//                                                                   (src: the file of a sample clip)
//     {op:"pattern.add", xml, clips:[{track, xml}]}                new pattern track + its clip in every
//                                                                   Pattern Editor track; appended as the last pattern
//     {op:"pattern.remove", id}
//     {op:"notes.set",   text}                                     project notes (HTML)
//     {op:"param.set",   owner, path, v}                           a knob/slider/button changed by hand
//         owner "song": path bpm | num | den | vol | pitch (tempo, time signature, master volume/pitch)
//         owner <trackId>: path "t:<n>" = n-th parameter of the track (instrument, envelopes, arpeggio...)
//         or "fx:<n>" = n-th parameter of its effect chain, in LMMS' construction order (the same on every
//         client, since all build the track from the same XML). The server only relays these...
//     {op:"track.state", id, xml}                                  ...and stores the track's settings element
//         (<instrumenttrack> / <sampletrack>) that the client of the latest change sends once knobs rest
//     {op:"instrument.set", track, xml}                            the track's whole <instrument> (other plugin,
//         preset, or plugin state that is not made of parameters: samples, ZynAddSubFX, VST...)
//     {op:"effects.set", track, xml}                               the track's whole <fxchain> (effects added,
//         removed or moved, or their inner state changed). Both are relayed; the sender adds a track.state.
//         effects.set can also carry "channel" instead of "track": the effects of a mixer channel.
//     Mixer (milestone M5a). Channels have ids ("cid" on <mixerchannel>; older projects: defaultChannelId());
//     the master channel is always first. These are relayed; with each of them the sender also sends
//     {op:"mixer.state", xml} (the whole <mixer>), which the server stores, as for knobs that come to rest.
//     {op:"mixer.add",    id, index}                               new channel (LMMS appends, then moves it)
//     {op:"mixer.remove", id}                                      tracks on it go to the master channel
//     {op:"mixer.order",  ids:[...]}                               order of the channels after the master
//     {op:"mixer.set",    id, v:{name, color, muted}}              any subset (solo is private, like tracks')
//     {op:"mixer.send",   from, to, on}                            a send between two channels appears/disappears
//     param.set with a channel as owner: path "c:0" = volume, "s:<channelId>" = amount sent to that channel,
//     "fx:<n>" = parameters of the channel's effects
//     Automation (milestone M5b). Automation tracks and clips use track.* and clip.* like the others.
//     {op:"automation.set", clip, prog, tens, nodes:[[pos, value, outValue, inTan, outTan, lockedTan]...],
//         objects:[{owner, path}...]}                              the whole content of an automation clip:
//         curve type (0 discrete, 1 linear, 2 cubic hermite), tension, nodes, and the automated parameters,
//         named like in param.set. In XML, <object> elements carry the same as "owner" / "path" attributes
//         (LMMS' own "id" differs on every client)
//     Controllers (milestone M5c). LFO controllers of the Controller Rack have ids ("cid"; older projects:
//     defaultControllerId()); Peak Controllers belong to their effect and are named "p:<ownerId>:<n>" (n-th
//     effect of a track or mixer channel); MIDI controllers are each user's own devices and never shared.
//     {op:"controller.add",    id, xml}                            a new LFO controller (<lfocontroller>)
//     {op:"controller.remove", id}
//     {op:"controller.set",    id, v:{name}}                       (a Peak Controller: "ref" instead of "id")
//     {op:"controllers.state", xml}                                the whole <controllers>, stored by the server
//     {op:"library.add", path, hash, size}                          a file uploaded by the sender is now shared
//     {op:"param.link", owner, path, controller}                   a parameter connected to a controller (its id or
//         "p:..." name), or disconnected (""). param.set with a controller as owner: path "k:<n>" = its knobs
// Tracks live in the "song" container (Song Editor) or the "patternstore" container (Pattern Editor).
// Pattern N is the N-th pattern track in song order; its content is the N-th clip of every Pattern Editor
// track, so reordering pattern tracks (track.order) also reorders those clips.
// clip/id/track are collaboration ids as 16 hex digits (see include/CollabId.h).

#ifndef LMMS_COLLAB_PROTOCOL_H
#define LMMS_COLLAB_PROTOCOL_H

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace lmms::collab::proto
{

inline constexpr int Version = 1;
inline constexpr quint16 DefaultPort = 42871;
//! Largest accepted frame (a full project snapshot must fit)
inline constexpr quint32 MaxFrameSize = 256u * 1024u * 1024u;

enum class FrameType : quint8
{
	Json = 0,
	Binary = 1
};

namespace msg
{
inline constexpr auto Hello = "hello";
inline constexpr auto Welcome = "welcome";
inline constexpr auto Create = "create";
inline constexpr auto Open = "open";
inline constexpr auto Joined = "joined";
inline constexpr auto Tx = "tx";
inline constexpr auto Presence = "presence";
inline constexpr auto Error = "error";
inline constexpr auto AssetPut = "asset.put";
inline constexpr auto AssetGet = "asset.get";
inline constexpr auto AssetStored = "asset.stored";
inline constexpr auto AssetData = "asset.data";
inline constexpr auto AssetError = "asset.error";
} // namespace msg

//! Largest shared file accepted
inline constexpr qint64 MaxAssetSize = 200ll * 1024 * 1024;
//! Largest data part of one binary frame
inline constexpr int AssetChunkSize = 256 * 1024;
//! Size of the SHA-256 at the start of every binary frame
inline constexpr int AssetHashSize = 32;
//! SHA-256 as 64 lowercase hex digits
bool isValidHash(const QString& hash);
//! A safe file name for a shared file (no folders, a known audio/instrument file type), or "" if unusable
QString sanitizeAssetName(const QString& name);
//! Encodes one binary frame: SHA-256 of the file + a part of its data
QByteArray encodeBinaryFrame(const QByteArray& hash, const QByteArray& data);

/**
 * Presence (decision M3): ephemeral, relayed by the server, never stored, never undone.
 *   cursor: null (not over a shared window) or
 *     {w: window, a?: anchor, tick?, key?, x?, y?}
 *       w: "song" | "pattern:<patternTrackId>" | "pianoroll:<clipId>" | "instrument:<trackId>"
 *          | "mixer" | "notes" | "controllers" | "effect:<ownerId>:<n>" (n-th effect of a track or mixer channel)
 *          | "automation:<clipId>" (Automation Editor: tick, y = value as a fraction of the automated range)
 *          | "toolbar" (the main toolbar: tempo, time signature...) | "controller:<id>" (an LFO's Controls)
 *       a: "track:<trackId>" (tick = time, y = 0..1 within the track row),
 *          "head:<trackId>" (x, y = pixels within the track's header),
 *          "chan:<n>" (x, y = pixels within mixer channel n), "fx:<n>" (pixels within the effects of channel n);
 *          none: x, y pixels in the window. Piano Roll: tick + key
 *       tab: the tab shown in an instrument window (the cursor is only drawn over the same tab)
 *   play: null or {song, pattern, pref?, playing, mode?, tick?, ref?}
 *       song / pattern: positions of the Song Editor and Pattern Editor timelines (where Play starts),
 *       pref: the pattern shown in the Pattern Editor,
 *       while playing: mode "song" | "pattern" | "clip", tick, ref (pattern track or clip id)
 *   view: null or {mixsel?: selected mixer channel}
 * Musical positions (tick/key/track/channel) look right regardless of each user's zoom and scroll.
 */
//! Returns a copy with only valid, known fields, or nullopt if the message is malformed
std::optional<QJsonObject> sanitizePresence(const QJsonObject& message);
//! "#rrggbb", or nullopt
std::optional<QString> validColor(const QJsonValue& value);

namespace op
{
inline constexpr auto NoteAdd = "note.add";
inline constexpr auto NoteSet = "note.set";
inline constexpr auto NoteRemove = "note.remove";
inline constexpr auto TrackAdd = "track.add";
inline constexpr auto TrackRemove = "track.remove";
inline constexpr auto TrackSet = "track.set";
inline constexpr auto TrackOrder = "track.order";
inline constexpr auto ClipAdd = "clip.add";
inline constexpr auto ClipRemove = "clip.remove";
inline constexpr auto ClipSet = "clip.set";
inline constexpr auto PatternAdd = "pattern.add";
inline constexpr auto PatternRemove = "pattern.remove";
inline constexpr auto NotesSet = "notes.set";
inline constexpr auto ParamSet = "param.set";
inline constexpr auto TrackState = "track.state";
inline constexpr auto InstrumentSet = "instrument.set";
inline constexpr auto EffectsSet = "effects.set";
inline constexpr auto MixerAdd = "mixer.add";
inline constexpr auto MixerRemove = "mixer.remove";
inline constexpr auto MixerOrder = "mixer.order";
inline constexpr auto MixerSet = "mixer.set";
inline constexpr auto MixerSend = "mixer.send";
inline constexpr auto MixerState = "mixer.state";
inline constexpr auto AutomationSet = "automation.set";
inline constexpr auto ControllerAdd = "controller.add";
inline constexpr auto ControllerRemove = "controller.remove";
inline constexpr auto ControllerSet = "controller.set";
inline constexpr auto ControllersState = "controllers.state";
inline constexpr auto ParamLink = "param.link";
inline constexpr auto LibraryAdd = "library.add";
} // namespace op

//! Id of the n-th controller in a project saved without controller ids.
//! Must match collab::defaultControllerId() in include/CollabId.h.
inline constexpr std::uint64_t defaultControllerId(int index)
{
	return 0x4354524c00000000ull + static_cast<std::uint64_t>(index);
}

//! "" (no controller), a controller id or "p:<ownerId>:<n>" (a Peak Controller effect)
bool isValidControllerRef(const QJsonValue& value);

//! Id of the mixer channel at @p index in a project saved without mixer channel ids.
//! Must match collab::defaultMixerChannelId() in include/CollabId.h.
inline constexpr std::uint64_t defaultChannelId(int index)
{
	return 0x4d49584300000000ull + static_cast<std::uint64_t>(index);
}

//! Owner of the song-wide parameters in param.set
inline constexpr auto SongOwner = "song";

//! A song-wide parameter: its path in param.set, its attribute in <head> and its range
struct SongParam
{
	const char* path;
	const char* attribute;
	int min;
	int max;
};
const std::vector<SongParam>& songParams();

//! "t:<n>", "i:<n>", "mt:<n>", "fx:<n>" (tracks), "c:0", "s:<channelId>" (mixer channels)
bool isValidParamPath(const QString& path);
//! A parameter's owner and path as in param.set (song-wide parameters: owner "song", path from songParams())
bool isValidParamRef(const QJsonValue& owner, const QJsonValue& path);
//! Checks the fields of automation.set (not the clip itself)
bool validAutomationContent(const QJsonObject& op);

//! Track containers: the Song Editor and the Pattern Editor
inline constexpr auto SongContainer = "song";
inline constexpr auto PatternContainer = "patternstore";
//! Largest project notes text accepted
inline constexpr int MaxNotesSize = 1024 * 1024;

//! Description of one synchronized attribute of a track or clip
struct FieldSpec
{
	enum class Kind { Int, Bool, String, Color, Id };
	const char* name;
	Kind kind;
	int min = 0;
	int max = 0;
};

//! Shared track attributes (solo is private, see decision D9)
const std::vector<FieldSpec>& trackFields();
//! Shared clip attributes; "steps" only exists for MIDI clips
const std::vector<FieldSpec>& clipFields();
//! Shared mixer channel attributes (solo is private)
const std::vector<FieldSpec>& mixerChannelFields();

//! Checks every present field of @p values against @p spec (unknown fields are invalid)
bool validFields(const QJsonObject& values, const std::vector<FieldSpec>& spec);

//! Encodes one JSON message as a complete frame
QByteArray encodeJsonFrame(const QJsonObject& message);

//! Incrementally splits a byte stream into frames
class FrameDecoder
{
public:
	void append(const QByteArray& data);
	//! Takes the next complete frame. Returns false if none is complete yet or the stream is invalid.
	bool next(FrameType& type, QByteArray& payload);
	//! The peer violated the framing (oversized frame or unknown type); the connection should be closed
	bool hasError() const { return m_error; }

private:
	QByteArray m_buffer;
	bool m_error = false;
};

//! Parses a JSON frame payload; returns nullopt unless it is a JSON object with a string "t"
std::optional<QJsonObject> parseMessage(const QByteArray& payload);

//! Fields of a note as transmitted. Unset fields are not part of a (partial) note.set.
struct NoteValues
{
	enum Field { Key, Pos, Len, Vol, Pan, Type, FieldCount };
	static constexpr std::array<const char*, FieldCount> FieldNames = {"key", "pos", "len", "vol", "pan", "type"};

	std::array<std::optional<int>, FieldCount> fields;

	std::optional<int>& operator[](Field f) { return fields[f]; }
	const std::optional<int>& operator[](Field f) const { return fields[f]; }

	bool isComplete() const;
	bool isEmpty() const;

	QJsonObject toJson() const;
	//! Returns nullopt if any present field has the wrong type or is out of range
	static std::optional<NoteValues> fromJson(const QJsonObject& obj);
	//! Range check for one field (same limits as LMMS: key 0..128, vol 0..200, pan -100..100, ...)
	static bool isValid(Field f, int value);
};

//! Collaboration ids travel as 16 hex digits; returns 0 if invalid
std::uint64_t parseId(const QJsonValue& value);
QString idString(std::uint64_t id);

} // namespace lmms::collab::proto

#endif // LMMS_COLLAB_PROTOCOL_H
