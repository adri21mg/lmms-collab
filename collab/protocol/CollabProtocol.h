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
//   server -> client
//     welcome {proto, clientId}
//     joined  {project, seq, mmp?}                   mmp is omitted for the creator (it already has the song)
//     tx      {seq, clientId, ctx, ops[]}            accepted transaction, sent to every client incl. sender
//     error   {message}
//
// Ops inside a tx (field names match LMMS' XML attributes):
//     {op:"note.add",    clip, id, v:{key,pos,len,vol,pan,type}}   all fields required
//     {op:"note.set",    clip, id, v:{any subset}}                 absolute values, never deltas
//     {op:"note.remove", clip, id}
//     {op:"track.add",   container:"song", index, xml}             complete <track> element (with its clips)
//     {op:"track.remove", id}
//     {op:"track.set",   id, v:{name, muted, color}}               any subset; color "" = no color
//     {op:"track.order", container:"song", ids:[...]}              order of the shared tracks
//     {op:"clip.add",    track, xml}                               complete clip element (with its notes)
//     {op:"clip.remove", id}
//     {op:"clip.set",    id, v:{pos,len,off,name,color,muted,autoresize,steps}}  any subset
//     {op:"pattern.add", xml, clips:[{track, xml}]}                new pattern track + its clip in every
//                                                                   Pattern Editor track; appended as the last pattern
//     {op:"pattern.remove", id}
//     {op:"notes.set",   text}                                     project notes (HTML)
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
inline constexpr auto Error = "error";
} // namespace msg

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
} // namespace op

//! Track containers: the Song Editor and the Pattern Editor
inline constexpr auto SongContainer = "song";
inline constexpr auto PatternContainer = "patternstore";
//! Largest project notes text accepted
inline constexpr int MaxNotesSize = 1024 * 1024;

//! Description of one synchronized attribute of a track or clip
struct FieldSpec
{
	enum class Kind { Int, Bool, String, Color };
	const char* name;
	Kind kind;
	int min = 0;
	int max = 0;
};

//! Shared track attributes (solo is private, see decision D9)
const std::vector<FieldSpec>& trackFields();
//! Shared clip attributes; "steps" only exists for MIDI clips
const std::vector<FieldSpec>& clipFields();

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
