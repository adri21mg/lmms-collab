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
#include <vector>

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
	bool applyPatternOp(const QString& type, const QJsonObject& op);
	bool applyNotesOp(const QJsonObject& op);
	bool applyParamOp(const QJsonObject& op);
	bool applyMixerOp(const QString& type, const QJsonObject& op);
	bool applyAutomationOp(const QJsonObject& op);
	bool applyControllerOp(const QString& type, const QJsonObject& op);
	//! Controller ids from the <controllers> element (ids added where missing)
	void indexControllers();
	bool isController(Id id) const { return id != 0 && (m_controllers.contains(id) || m_announcedControllers.contains(id)); }
	//! A track, mixer channel or controller: something that owns synchronized parameters
	bool isParamOwner(Id id) const;
	//! Mixer channel ids by position, from the <mixer> element (ids added where missing)
	void indexMixer();
	//! Settings element of an instrument or sample track (it holds the "mixch" channel number)
	static QDomElement trackSettings(const QDomElement& track);
	//! A channel of the mixer or one announced by mixer.add
	bool isChannel(Id id) const;

	//! Container element and track set for "song" / "patternstore"; null if unknown
	QDomElement containerElement(const QString& name) const;
	//! Pattern tracks of the Song Editor in order; pattern N is the N-th of them
	std::vector<QDomElement> patternTracks() const;
	//! Pattern Editor tracks in order
	std::vector<QDomElement> patternEditorTracks() const;
	//! Length of one bar in ticks (Pattern Editor clip N starts at bar N)
	int ticksPerBar() const;
	//! After pattern tracks were reordered: moves the Pattern Editor clips along with their pattern
	void permutePatternClips(const std::vector<Id>& oldPatternOrder);
	void removeTrackElement(Id trackId);

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
	QDomElement m_patternContainer;              // inside one of the pattern tracks
	QHash<Id, QDomElement> m_tracks;             // every track by id
	QSet<Id> m_songTracks;                       // tracks of the Song Editor
	QSet<Id> m_patternEditorTracks;              // tracks of the Pattern Editor
	QHash<Id, QDomElement> m_clips;              // clip elements by id
	QHash<Id, Id> m_clipTrack;                   // track id of each clip
	QHash<Id, QHash<Id, QDomElement>> m_notes;   // note elements by clip id, note id (MIDI clips only)
	QSet<Id> m_unsortedClips;
	QDomElement m_mixer;                         // <mixer> of the song
	std::vector<Id> m_channels;                  // mixer channel ids by position (0 = master)
	QSet<Id> m_announcedChannels;                // added by mixer.add, until the mixer.state that follows
	QSet<Id> m_controllers;                      // controllers of the Controller Rack
	QSet<Id> m_announcedControllers;             // added by controller.add, until the controllers.state that follows
};

} // namespace lmms::collab

#endif // LMMS_COLLAB_PROJECT_STATE_H
