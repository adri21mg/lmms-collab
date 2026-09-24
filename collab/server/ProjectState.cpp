/*
 * ProjectState.cpp - authoritative state of one shared project (collaboration server)
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

#include "ProjectState.h"

#include <algorithm>
#include <vector>

#include <QJsonArray>

#include "CollabProtocol.h"

namespace lmms::collab
{

namespace
{

//! Direct children of <song> that only describe one user's editors, never shared (decision D9)
const QStringList PrivateSongElements = {"pianoroll", "automationeditor", "ControllerRackView", "timeline"};
//! Window geometry LMMS stores on some elements (e.g. the instrument window inside <instrumenttrack>)
const QStringList WindowAttributes = {"x", "y", "width", "height", "visible", "maximized", "minimized", "tab"};
const QStringList WindowStateElements = {"trackcontainer", "instrumenttrack", "sampletrack"};

// Track types (Track::Type) and the clip element each one holds
constexpr int InstrumentTrack = 0;
constexpr int PatternTrack = 1;
constexpr int SampleTrack = 2;

QString clipTagFor(int trackType)
{
	switch (trackType)
	{
	case InstrumentTrack: return "midiclip";
	case PatternTrack: return "patternclip";
	case SampleTrack: return "sampleclip";
	default: return {};
	}
}

bool isClipTag(const QString& tag)
{
	return tag == "midiclip" || tag == "patternclip" || tag == "sampleclip" || tag == "automationclip";
}

using proto::NoteValues;

int noteAttr(const QDomElement& note, NoteValues::Field f)
{
	return note.attribute(NoteValues::FieldNames[f]).toInt();
}

std::vector<QDomElement> childTracks(const QDomElement& container)
{
	std::vector<QDomElement> tracks;
	for (QDomElement t = container.firstChildElement("track"); !t.isNull(); t = t.nextSiblingElement("track"))
	{
		tracks.push_back(t);
	}
	return tracks;
}

//! Solo is private: the shared mute of each track is the one it had before any solo
void normalizeSolo(const std::vector<QDomElement>& tracks)
{
	const bool anySolo = std::any_of(tracks.begin(), tracks.end(),
		[](const QDomElement& t) { return t.attribute("solo") == "1"; });
	for (QDomElement t : tracks)
	{
		if (anySolo && t.hasAttribute("mutedBeforeSolo")) { t.setAttribute("muted", t.attribute("mutedBeforeSolo")); }
		t.setAttribute("solo", "0");
	}
}

//! Writes validated field values (see proto::FieldSpec) as XML attributes
void setFields(QDomElement& element, const QJsonObject& values, const std::vector<proto::FieldSpec>& spec)
{
	for (auto it = values.begin(); it != values.end(); ++it)
	{
		const auto f = std::find_if(spec.begin(), spec.end(), [&](const auto& s) { return it.key() == s.name; });
		switch (f->kind)
		{
		case proto::FieldSpec::Kind::Int: element.setAttribute(it.key(), it.value().toInt()); break;
		case proto::FieldSpec::Kind::Bool: element.setAttribute(it.key(), it.value().toBool() ? 1 : 0); break;
		case proto::FieldSpec::Kind::String: element.setAttribute(it.key(), it.value().toString()); break;
		case proto::FieldSpec::Kind::Color:
			if (it.value().toString().isEmpty()) { element.removeAttribute(it.key()); }
			else { element.setAttribute(it.key(), it.value().toString()); }
			break;
		}
	}
}

} // namespace


bool ProjectState::load(const QByteArray& mmp, QString& error)
{
	QDomDocument doc;
	const auto result = doc.setContent(mmp);
	if (!result)
	{
		error = QString{"invalid XML at line %1: %2"}.arg(result.errorLine).arg(result.errorMessage);
		return false;
	}
	const QDomElement root = doc.documentElement();
	if (root.tagName() != "lmms-project" || root.firstChildElement("song").isNull())
	{
		error = "not an LMMS song project";
		return false;
	}
	m_doc = doc;
	normalize(m_doc.documentElement());
	index();
	return true;
}


void ProjectState::normalize(QDomElement root)
{
	if (root.tagName() == "lmms-project")
	{
		QDomElement song = root.firstChildElement("song");
		for (const QString& name : PrivateSongElements)
		{
			for (QDomElement e = song.firstChildElement(name); !e.isNull(); e = song.firstChildElement(name))
			{
				song.removeChild(e);
			}
		}
	}

	auto stripWindowState = [](QDomElement e) {
		for (const QString& a : WindowAttributes) { e.removeAttribute(a); }
	};
	if (WindowStateElements.contains(root.tagName())) { stripWindowState(root); }
	for (const QString& tag : WindowStateElements)
	{
		const QDomNodeList list = root.elementsByTagName(tag);
		for (int i = 0; i < list.size(); ++i) { stripWindowState(list.at(i).toElement()); }
	}

	if (root.tagName() == "track") { normalizeSolo({root}); }
	const QDomNodeList containers = root.elementsByTagName("trackcontainer");
	for (int i = 0; i < containers.size(); ++i) { normalizeSolo(childTracks(containers.at(i).toElement())); }
}


void ProjectState::index()
{
	m_tracks.clear();
	m_songTracks.clear();
	m_clips.clear();
	m_clipTrack.clear();
	m_notes.clear();
	m_unsortedClips.clear();

	QDomElement song = m_doc.documentElement().firstChildElement("song");
	for (QDomElement c = song.firstChildElement("trackcontainer"); !c.isNull(); c = c.nextSiblingElement("trackcontainer"))
	{
		if (c.attribute("type") == proto::SongContainer) { m_songContainer = c; }
	}
	for (const QDomElement& track : childTracks(m_songContainer)) { indexTrack(track, true); }
}


void ProjectState::indexTrack(const QDomElement& track, bool inSong)
{
	const Id trackId = proto::parseId(track.attribute("cid"));
	if (trackId == 0 || m_tracks.contains(trackId)) { return; } // not addressable
	m_tracks.insert(trackId, track);
	if (inSong) { m_songTracks.insert(trackId); }

	for (QDomElement e = track.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
	{
		if (isClipTag(e.tagName())) { indexClip(e, trackId); }
		else if (e.tagName() == "patterntrack")
		{
			// The Pattern Editor's tracks are stored inside the first pattern track
			for (QDomElement c = e.firstChildElement("trackcontainer"); !c.isNull(); c = c.nextSiblingElement("trackcontainer"))
			{
				for (const QDomElement& t : childTracks(c)) { indexTrack(t, false); }
			}
		}
	}
}


void ProjectState::indexClip(const QDomElement& clip, Id trackId)
{
	const Id clipId = proto::parseId(clip.attribute("cid"));
	if (clipId == 0 || m_clips.contains(clipId)) { return; }
	m_clips.insert(clipId, clip);
	m_clipTrack.insert(clipId, trackId);
	if (clip.tagName() != "midiclip") { return; }
	auto& notes = m_notes[clipId];
	for (QDomElement n = clip.firstChildElement("note"); !n.isNull(); n = n.nextSiblingElement("note"))
	{
		const Id noteId = proto::parseId(n.attribute("cid"));
		if (noteId != 0) { notes.insert(noteId, n); }
	}
}


void ProjectState::unindexClip(Id clipId)
{
	m_clips.remove(clipId);
	m_clipTrack.remove(clipId);
	m_notes.remove(clipId);
	m_unsortedClips.remove(clipId);
}


QDomElement ProjectState::parseFragment(const QJsonValue& xml, const QString& expectedTag)
{
	if (!xml.isString()) { return {}; }
	QDomDocument fragment;
	if (!fragment.setContent(xml.toString())) { return {}; }
	QDomElement root = fragment.documentElement();
	return root.tagName() == expectedTag && proto::parseId(root.attribute("cid")) != 0 ? root : QDomElement{};
}


bool ProjectState::idsAreNew(const QDomElement& element) const
{
	QSet<Id> seen;
	auto check = [&](const QDomElement& e) {
		const bool isTrack = e.tagName() == "track";
		if (!isTrack && !isClipTag(e.tagName())) { return true; }
		const Id id = proto::parseId(e.attribute("cid"));
		if (id == 0 || seen.contains(id)) { return false; }
		seen.insert(id);
		return isTrack ? !m_tracks.contains(id) : !m_clips.contains(id);
	};
	if (!check(element)) { return false; }
	const QDomNodeList all = element.elementsByTagName("*");
	for (int i = 0; i < all.size(); ++i)
	{
		if (!check(all.at(i).toElement())) { return false; }
	}
	return true;
}


bool ProjectState::apply(const QJsonObject& op)
{
	const QString type = op.value("op").toString();
	if (type.startsWith("note.")) { return applyNoteOp(type, op); }
	if (type.startsWith("track.")) { return applyTrackOp(type, op); }
	if (type.startsWith("clip.")) { return applyClipOp(type, op); }
	return false;
}


bool ProjectState::applyNoteOp(const QString& type, const QJsonObject& op)
{
	const Id clipId = proto::parseId(op.value("clip"));
	const Id noteId = proto::parseId(op.value("id"));
	if (clipId == 0 || noteId == 0 || !m_notes.contains(clipId)) { return false; }

	QDomElement clip = m_clips.value(clipId);
	auto& notes = m_notes[clipId];

	if (type == proto::op::NoteRemove)
	{
		if (!notes.contains(noteId)) { return false; }
		clip.removeChild(notes.take(noteId));
		return true;
	}

	const auto values = NoteValues::fromJson(op.value("v").toObject());
	if (!values || values->isEmpty()) { return false; }

	QDomElement note;
	if (type == proto::op::NoteAdd)
	{
		if (!values->isComplete() || notes.contains(noteId)) { return false; }
		note = m_doc.createElement("note");
		note.setAttribute("cid", proto::idString(noteId));
		clip.appendChild(note);
		notes.insert(noteId, note);
	}
	else if (type == proto::op::NoteSet)
	{
		if (!notes.contains(noteId)) { return false; }
		note = notes.value(noteId);
	}
	else
	{
		return false;
	}

	for (int f = 0; f < NoteValues::FieldCount; ++f)
	{
		if (const auto& v = values->fields[f]) { note.setAttribute(NoteValues::FieldNames[f], *v); }
	}
	m_unsortedClips.insert(clipId);
	return true;
}


bool ProjectState::applyTrackOp(const QString& type, const QJsonObject& op)
{
	if (m_songContainer.isNull()) { return false; }

	if (type == proto::op::TrackAdd)
	{
		if (op.value("container").toString() != proto::SongContainer) { return false; }
		QDomElement track = parseFragment(op.value("xml"), "track");
		const int trackType = track.attribute("type", "-1").toInt();
		if (track.isNull() || (trackType != InstrumentTrack && trackType != SampleTrack) || !idsAreNew(track))
		{
			return false;
		}
		normalize(track);
		QDomElement imported = m_doc.importNode(track, true).toElement();
		const auto tracks = childTracks(m_songContainer);
		const int index = op.value("index").toInt(static_cast<int>(tracks.size()));
		if (index >= 0 && index < static_cast<int>(tracks.size())) { m_songContainer.insertBefore(imported, tracks[index]); }
		else { m_songContainer.appendChild(imported); }
		indexTrack(imported, true);
		return true;
	}

	if (type == proto::op::TrackOrder)
	{
		if (op.value("container").toString() != proto::SongContainer) { return false; }
		std::vector<QDomElement> wanted;
		QSet<Id> listed;
		for (const QJsonValue& v : op.value("ids").toArray())
		{
			const Id id = proto::parseId(v);
			if (!m_songTracks.contains(id) || listed.contains(id)) { return false; }
			listed.insert(id);
			wanted.push_back(m_tracks.value(id));
		}
		// Listed tracks take the slots the listed tracks occupy now, in the given order
		auto tracks = childTracks(m_songContainer);
		std::size_t next = 0;
		for (auto& t : tracks)
		{
			if (listed.contains(proto::parseId(t.attribute("cid")))) { t = wanted[next++]; }
		}
		for (const auto& t : tracks) { m_songContainer.appendChild(t); } // appendChild moves existing nodes
		return true;
	}

	const Id trackId = proto::parseId(op.value("id"));
	if (!m_songTracks.contains(trackId)) { return false; }
	QDomElement track = m_tracks.value(trackId);

	if (type == proto::op::TrackRemove)
	{
		const int trackType = track.attribute("type").toInt();
		if (trackType != InstrumentTrack && trackType != SampleTrack) { return false; }
		for (const Id clipId : m_clipTrack.keys(trackId)) { unindexClip(clipId); }
		m_songContainer.removeChild(track);
		m_tracks.remove(trackId);
		m_songTracks.remove(trackId);
		return true;
	}

	if (type == proto::op::TrackSet)
	{
		QJsonObject values = op.value("v").toObject();
		if (values.isEmpty() || !proto::validFields(values, proto::trackFields())) { return false; }
		setFields(track, values, proto::trackFields());
		if (values.contains("muted")) { track.setAttribute("mutedBeforeSolo", values.value("muted").toBool() ? 1 : 0); }
		return true;
	}
	return false;
}


bool ProjectState::applyClipOp(const QString& type, const QJsonObject& op)
{
	if (type == proto::op::ClipAdd)
	{
		const Id trackId = proto::parseId(op.value("track"));
		if (!m_songTracks.contains(trackId)) { return false; }
		QDomElement track = m_tracks.value(trackId);
		const QString tag = clipTagFor(track.attribute("type").toInt());
		if (tag.isEmpty()) { return false; }
		QDomElement clip = parseFragment(op.value("xml"), tag);
		if (clip.isNull() || !idsAreNew(clip)) { return false; }
		QDomElement imported = m_doc.importNode(clip, true).toElement();
		track.appendChild(imported);
		indexClip(imported, trackId);
		return true;
	}

	const Id clipId = proto::parseId(op.value("id"));
	// Only clips of the Song Editor's tracks; the Pattern Editor's structure is not shared yet
	if (!m_clips.contains(clipId) || !m_songTracks.contains(m_clipTrack.value(clipId))) { return false; }
	QDomElement clip = m_clips.value(clipId);
	if (clip.tagName() == "automationclip") { return false; }

	if (type == proto::op::ClipRemove)
	{
		m_tracks.value(m_clipTrack.value(clipId)).removeChild(clip);
		unindexClip(clipId);
		return true;
	}

	if (type == proto::op::ClipSet)
	{
		const QJsonObject values = op.value("v").toObject();
		if (values.isEmpty() || !proto::validFields(values, proto::clipFields())) { return false; }
		if (values.contains("steps") && clip.tagName() != "midiclip") { return false; }
		setFields(clip, values, proto::clipFields());
		return true;
	}
	return false;
}


void ProjectState::sortNotes(QDomElement& clip)
{
	std::vector<QDomElement> notes;
	for (QDomElement n = clip.firstChildElement("note"); !n.isNull(); n = n.nextSiblingElement("note"))
	{
		notes.push_back(n);
	}
	// Same order as Note::lessThan: position ascending, then key descending
	std::stable_sort(notes.begin(), notes.end(), [](const QDomElement& a, const QDomElement& b) {
		const int pa = noteAttr(a, NoteValues::Pos), pb = noteAttr(b, NoteValues::Pos);
		return pa != pb ? pa < pb : noteAttr(a, NoteValues::Key) > noteAttr(b, NoteValues::Key);
	});
	for (auto& n : notes) { clip.appendChild(n); } // appendChild moves an existing child to the end
}


QByteArray ProjectState::toMmp()
{
	for (const Id clipId : m_unsortedClips)
	{
		QDomElement clip = m_clips.value(clipId);
		sortNotes(clip);
	}
	m_unsortedClips.clear();
	return m_doc.toByteArray(1);
}

} // namespace lmms::collab
