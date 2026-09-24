/*
 * ProjectState.h - authoritative state of one shared project (collaboration server)
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

#ifndef LMMS_COLLAB_PROJECT_STATE_H
#define LMMS_COLLAB_PROJECT_STATE_H

#include <cstdint>

#include <QDomDocument>
#include <QHash>
#include <QJsonObject>
#include <QSet>

namespace lmms::collab
{

//! The live project as LMMS' own .mmp XML. Operations are applied directly to the XML, so the server
//! needs neither the LMMS engine nor any plugin, and a snapshot is simply the document written out.
class ProjectState
{
public:
	//! Parses a project sent by a client or read from disk. Private state (editor windows, loop points,
	//! solo) is removed, see normalize(). Returns false with @p error set if it is not a usable project.
	bool load(const QByteArray& mmp, QString& error);

	//! Validates and applies one operation. Invalid or stale operations (unknown objects, bad values)
	//! are rejected without changing anything; that is how last-write-wins resolves e.g. edit-after-delete.
	bool apply(const QJsonObject& op);

	//! Current state as .mmp XML (notes of every clip sorted the way LMMS expects)
	QByteArray toMmp();

	int clipCount() const { return m_clips.size(); }

private:
	using Id = std::uint64_t;

	bool applyNoteOp(const QString& type, const QJsonObject& op);
	bool applyTrackOp(const QString& type, const QJsonObject& op);
	bool applyClipOp(const QString& type, const QJsonObject& op);

	//! Removes private state from a project or from a track sent by a client
	static void normalize(QDomElement root);
	void index();
	void indexTrack(const QDomElement& track, bool inSong);
	void indexClip(const QDomElement& clip, Id trackId);
	void unindexClip(Id clipId);
	//! Parses a single element sent by a client; null element if invalid
	static QDomElement parseFragment(const QJsonValue& xml, const QString& expectedTag);
	//! True if the element (and everything with a cid inside it) uses ids not known yet
	bool idsAreNew(const QDomElement& element) const;
	void sortNotes(QDomElement& clip);

	QDomDocument m_doc;
	QDomElement m_songContainer;
	QHash<Id, QDomElement> m_tracks;             // every track by id
	QSet<Id> m_songTracks;                       // tracks of the Song Editor (structure is shared)
	QHash<Id, QDomElement> m_clips;              // clip elements by id
	QHash<Id, Id> m_clipTrack;                   // track id of each clip
	QHash<Id, QHash<Id, QDomElement>> m_notes;   // note elements by clip id, note id (MIDI clips only)
	QSet<Id> m_unsortedClips;
};

} // namespace lmms::collab

#endif // LMMS_COLLAB_PROJECT_STATE_H
