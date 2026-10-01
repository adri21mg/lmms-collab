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
#include <cmath>

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
const QStringList WindowStateElements = {"trackcontainer", "instrumenttrack", "sampletrack", "projectnotes"};

// Track types (Track::Type) and the clip element each one holds
constexpr int InstrumentTrack = 0;
constexpr int PatternTrack = 1;
constexpr int SampleTrack = 2;
constexpr int AutomationTrack = 5;

QString clipTagFor(int trackType)
{
	switch (trackType)
	{
	case InstrumentTrack: return "midiclip";
	case PatternTrack: return "patternclip";
	case SampleTrack: return "sampleclip";
	case AutomationTrack: return "automationclip";
	default: return {};
	}
}

bool isClipTag(const QString& tag)
{
	return tag == "midiclip" || tag == "patternclip" || tag == "sampleclip" || tag == "automationclip";
}

bool isContentTrackType(int type)
{
	return type == InstrumentTrack || type == SampleTrack || type == AutomationTrack;
}

using proto::NoteValues;
using Id = std::uint64_t;

int noteAttr(const QDomElement& note, NoteValues::Field f)
{
	return note.attribute(NoteValues::FieldNames[f]).toInt();
}

Id idOf(const QDomElement& e)
{
	return proto::parseId(e.attribute("cid"));
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

std::vector<QDomElement> childClips(const QDomElement& track)
{
	std::vector<QDomElement> clips;
	for (QDomElement e = track.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
	{
		if (isClipTag(e.tagName())) { clips.push_back(e); }
	}
	return clips;
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
		case proto::FieldSpec::Kind::Id: element.setAttribute(it.key(), it.value().toString()); break;
		}
	}
}

} // namespace


bool ProjectState::load(const QByteArray& mmp, QString& error)
{
	QDomDocument doc;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	const auto result = doc.setContent(mmp);
	if (!result)
	{
		error = QString{"invalid XML at line %1: %2"}.arg(result.errorLine).arg(result.errorMessage);
		return false;
	}
#else // e.g. Ubuntu 24.04 (Qt 6.4)
	QString message;
	int line = 0;
	if (!doc.setContent(mmp, &message, &line))
	{
		error = QString{"invalid XML at line %1: %2"}.arg(line).arg(message);
		return false;
	}
#endif
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
	// Mixer solo is private too (LMMS does not keep the mute from before a solo there)
	if (root.tagName() == "mixerchannel") { root.setAttribute("soloed", "0"); }
	const QDomNodeList channels = root.elementsByTagName("mixerchannel");
	for (int i = 0; i < channels.size(); ++i) { channels.at(i).toElement().setAttribute("soloed", "0"); }
	const QDomNodeList containers = root.elementsByTagName("trackcontainer");
	for (int i = 0; i < containers.size(); ++i) { normalizeSolo(childTracks(containers.at(i).toElement())); }
}


void ProjectState::index()
{
	m_tracks.clear();
	m_songTracks.clear();
	m_patternEditorTracks.clear();
	m_clips.clear();
	m_clipTrack.clear();
	m_notes.clear();
	m_unsortedClips.clear();
	m_songContainer = QDomElement{};
	m_patternContainer = QDomElement{};

	QDomElement song = m_doc.documentElement().firstChildElement("song");
	for (QDomElement c = song.firstChildElement("trackcontainer"); !c.isNull(); c = c.nextSiblingElement("trackcontainer"))
	{
		if (c.attribute("type") == proto::SongContainer) { m_songContainer = c; }
	}
	for (const QDomElement& track : childTracks(m_songContainer)) { indexTrack(track, true); }
	indexMixer();
	indexControllers();
}


void ProjectState::indexControllers()
{
	m_controllers.clear();
	m_announcedControllers.clear();
	int index = 0;
	QDomElement controllers = m_doc.documentElement().firstChildElement("song").firstChildElement("controllers");
	for (QDomElement c = controllers.firstChildElement(); !c.isNull(); c = c.nextSiblingElement(), ++index)
	{
		if (idOf(c) == 0) { c.setAttribute("cid", proto::idString(proto::defaultControllerId(index))); }
		m_controllers.insert(idOf(c));
	}
}


bool ProjectState::isParamOwner(Id id) const
{
	return m_songTracks.contains(id) || m_patternEditorTracks.contains(id) || isChannel(id) || isController(id);
}


void ProjectState::indexMixer()
{
	m_channels.clear();
	m_announcedChannels.clear();
	m_mixer = m_doc.documentElement().firstChildElement("song").firstChildElement("mixer");
	for (QDomElement c = m_mixer.firstChildElement("mixerchannel"); !c.isNull(); c = c.nextSiblingElement("mixerchannel"))
	{
		const int num = c.attribute("num").toInt();
		if (num < 0 || num > 10000) { continue; }
		if (idOf(c) == 0) { c.setAttribute("cid", proto::idString(proto::defaultChannelId(num))); }
		if (static_cast<std::size_t>(num) >= m_channels.size()) { m_channels.resize(num + 1, 0); }
		m_channels[num] = idOf(c);
	}
	if (m_channels.empty()) { m_channels.push_back(proto::defaultChannelId(0)); } // just the master
}


bool ProjectState::isChannel(Id id) const
{
	return id != 0 && (std::find(m_channels.begin(), m_channels.end(), id) != m_channels.end()
		|| m_announcedChannels.contains(id));
}


QDomElement ProjectState::trackSettings(const QDomElement& track)
{
	const int type = track.attribute("type").toInt();
	if (type == InstrumentTrack) { return track.firstChildElement("instrumenttrack"); }
	if (type == SampleTrack) { return track.firstChildElement("sampletrack"); }
	return {};
}


void ProjectState::indexTrack(const QDomElement& track, bool inSong)
{
	const Id trackId = idOf(track);
	if (trackId == 0 || m_tracks.contains(trackId)) { return; } // not addressable
	m_tracks.insert(trackId, track);
	(inSong ? m_songTracks : m_patternEditorTracks).insert(trackId);

	for (QDomElement e = track.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
	{
		if (isClipTag(e.tagName())) { indexClip(e, trackId); }
		else if (e.tagName() == "patterntrack")
		{
			// The Pattern Editor's tracks are stored inside one of the pattern tracks
			for (QDomElement c = e.firstChildElement("trackcontainer"); !c.isNull(); c = c.nextSiblingElement("trackcontainer"))
			{
				if (c.attribute("type") != proto::PatternContainer || !m_patternContainer.isNull()) { continue; }
				m_patternContainer = c;
				for (const QDomElement& t : childTracks(c)) { indexTrack(t, false); }
			}
		}
	}
}


void ProjectState::indexClip(const QDomElement& clip, Id trackId)
{
	const Id clipId = idOf(clip);
	if (clipId == 0 || m_clips.contains(clipId)) { return; }
	m_clips.insert(clipId, clip);
	m_clipTrack.insert(clipId, trackId);
	if (clip.tagName() != "midiclip") { return; }
	auto& notes = m_notes[clipId];
	for (QDomElement n = clip.firstChildElement("note"); !n.isNull(); n = n.nextSiblingElement("note"))
	{
		const Id noteId = idOf(n);
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
	return root.tagName() == expectedTag && idOf(root) != 0 ? root : QDomElement{};
}


bool ProjectState::idsAreNew(const QDomElement& element) const
{
	QSet<Id> seen;
	auto check = [&](const QDomElement& e) {
		const bool isTrack = e.tagName() == "track";
		if (!isTrack && !isClipTag(e.tagName())) { return true; }
		const Id id = idOf(e);
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


QDomElement ProjectState::containerElement(const QString& name) const
{
	if (name == proto::SongContainer) { return m_songContainer; }
	if (name == proto::PatternContainer) { return m_patternContainer; }
	return {};
}


std::vector<QDomElement> ProjectState::patternTracks() const
{
	std::vector<QDomElement> result;
	for (const QDomElement& t : childTracks(m_songContainer))
	{
		if (t.attribute("type").toInt() == PatternTrack) { result.push_back(t); }
	}
	return result;
}


std::vector<QDomElement> ProjectState::patternEditorTracks() const
{
	return m_patternContainer.isNull() ? std::vector<QDomElement>{} : childTracks(m_patternContainer);
}


int ProjectState::ticksPerBar() const
{
	// Same as TimePos::ticksPerBar(): 192 ticks for a 4/4 bar
	const QDomElement head = m_doc.documentElement().firstChildElement("head");
	const int numerator = std::max(1, head.attribute("timesig_numerator", "4").toInt());
	const int denominator = std::max(1, head.attribute("timesig_denominator", "4").toInt());
	return 192 * numerator / denominator;
}


void ProjectState::removeTrackElement(Id trackId)
{
	QDomElement track = m_tracks.value(trackId);
	for (const Id clipId : m_clipTrack.keys(trackId)) { unindexClip(clipId); }
	track.parentNode().removeChild(track);
	m_tracks.remove(trackId);
	m_songTracks.remove(trackId);
	m_patternEditorTracks.remove(trackId);
}


bool ProjectState::apply(const QJsonObject& op)
{
	const QString type = op.value("op").toString();
	if (type.startsWith("note.")) { return applyNoteOp(type, op); }
	if (type.startsWith("track.")) { return applyTrackOp(type, op); }
	if (type.startsWith("clip.")) { return applyClipOp(type, op); }
	if (type.startsWith("pattern.")) { return applyPatternOp(type, op); }
	if (type == proto::op::NotesSet) { return applyNotesOp(op); }
	if (type == proto::op::ParamSet) { return applyParamOp(op); }
	if (type.startsWith("mixer.")) { return applyMixerOp(type, op); }
	if (type == proto::op::AutomationSet) { return applyAutomationOp(op); }
	if (type.startsWith("controller") || type == proto::op::ParamLink) { return applyControllerOp(type, op); }
	if (type == proto::op::EffectsSet && op.contains("channel"))
	{
		// A mixer channel's effects: relayed, stored with the mixer.state that comes with them
		QDomDocument fragment;
		return isChannel(proto::parseId(op.value("channel"))) && op.value("xml").isString()
			&& fragment.setContent(op.value("xml").toString()) && fragment.documentElement().tagName() == "fxchain";
	}
	if (type == proto::op::InstrumentSet || type == proto::op::EffectsSet)
	{
		// Relayed to the other clients; the sender stores the result with a track.state in the same tx
		const Id trackId = proto::parseId(op.value("track"));
		const bool known = m_songTracks.contains(trackId) || m_patternEditorTracks.contains(trackId);
		QDomDocument fragment;
		if (!known || !op.value("xml").isString() || !fragment.setContent(op.value("xml").toString())) { return false; }
		const QString expected = type == proto::op::InstrumentSet ? "instrument" : "fxchain";
		const int trackType = m_tracks.value(trackId).attribute("type").toInt();
		return fragment.documentElement().tagName() == expected
			&& (trackType == InstrumentTrack || (trackType == SampleTrack && expected == "fxchain"));
	}
	return false;
}


bool ProjectState::applyParamOp(const QJsonObject& op)
{
	const QJsonValue value = op.value("v");
	if (!value.isDouble() || !std::isfinite(value.toDouble())) { return false; }
	const QString path = op.value("path").toString();

	if (op.value("owner").toString() == proto::SongOwner)
	{
		const auto& params = proto::songParams();
		const auto param = std::find_if(params.begin(), params.end(), [&](const auto& p) { return path == p.path; });
		const double v = value.toDouble();
		if (param == params.end() || v != std::floor(v) || v < param->min || v > param->max) { return false; }
		m_doc.documentElement().firstChildElement("head").setAttribute(param->attribute, static_cast<int>(v));
		return true;
	}

	// Track and mixer channel parameters are only relayed; track.state / mixer.state store them
	const Id owner = proto::parseId(op.value("owner"));
	return isParamOwner(owner) && proto::isValidParamPath(path);
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
	if (type == proto::op::TrackAdd)
	{
		const QString containerName = op.value("container").toString();
		QDomElement container = containerElement(containerName);
		QDomElement track = parseFragment(op.value("xml"), "track");
		if (container.isNull() || track.isNull() || !isContentTrackType(track.attribute("type", "-1").toInt())
			|| !idsAreNew(track))
		{
			return false;
		}
		const bool inPatternEditor = containerName == proto::PatternContainer;
		// A Pattern Editor track holds exactly one clip per pattern
		if (inPatternEditor && childClips(track).size() != patternTracks().size()) { return false; }
		normalize(track);
		QDomElement imported = m_doc.importNode(track, true).toElement();
		const auto tracks = childTracks(container);
		const int index = op.value("index").toInt(-1);
		if (index >= 0 && index < static_cast<int>(tracks.size())) { container.insertBefore(imported, tracks[index]); }
		else { container.appendChild(imported); }
		indexTrack(imported, !inPatternEditor);
		return true;
	}

	if (type == proto::op::TrackOrder)
	{
		const QString containerName = op.value("container").toString();
		QDomElement container = containerElement(containerName);
		if (container.isNull()) { return false; }
		const QSet<Id>& members = containerName == proto::SongContainer ? m_songTracks : m_patternEditorTracks;
		std::vector<QDomElement> wanted;
		QSet<Id> listed;
		for (const QJsonValue& v : op.value("ids").toArray())
		{
			const Id id = proto::parseId(v);
			if (!members.contains(id) || listed.contains(id)) { return false; }
			listed.insert(id);
			wanted.push_back(m_tracks.value(id));
		}
		std::vector<Id> oldPatternOrder;
		for (const QDomElement& p : patternTracks()) { oldPatternOrder.push_back(idOf(p)); }

		// Listed tracks take the slots the listed tracks occupy now, in the given order
		auto tracks = childTracks(container);
		std::size_t next = 0;
		for (auto& t : tracks)
		{
			if (listed.contains(idOf(t))) { t = wanted[next++]; }
		}
		for (const auto& t : tracks) { container.appendChild(t); } // appendChild moves existing nodes
		if (containerName == proto::SongContainer) { permutePatternClips(oldPatternOrder); }
		return true;
	}

	const Id trackId = proto::parseId(op.value("id"));
	if (!m_songTracks.contains(trackId) && !m_patternEditorTracks.contains(trackId)) { return false; }
	QDomElement track = m_tracks.value(trackId);

	if (type == proto::op::TrackRemove)
	{
		// Pattern tracks are removed with pattern.remove
		if (!isContentTrackType(track.attribute("type").toInt())) { return false; }
		removeTrackElement(trackId);
		return true;
	}

	if (type == proto::op::TrackState)
	{
		// The settings element of the track (instrument, its parameters and effects), replacing the old one
		const int trackType = track.attribute("type").toInt();
		const QString tag = trackType == InstrumentTrack ? "instrumenttrack" : trackType == SampleTrack ? "sampletrack" : "";
		if (tag.isEmpty()) { return false; }
		QDomDocument fragment;
		if (!op.value("xml").isString() || !fragment.setContent(op.value("xml").toString())) { return false; }
		QDomElement settings = fragment.documentElement();
		if (settings.tagName() != tag || !settings.elementsByTagName("track").isEmpty()) { return false; }
		normalize(settings);
		QDomElement imported = m_doc.importNode(settings, true).toElement();
		const QDomElement old = track.firstChildElement(tag);
		if (old.isNull()) { track.insertBefore(imported, track.firstChild()); }
		else { track.replaceChild(imported, old); }
		return true;
	}

	if (type == proto::op::TrackSet)
	{
		QJsonObject values = op.value("v").toObject();
		if (values.isEmpty() || !proto::validFields(values, proto::trackFields())) { return false; }
		if (values.contains("channel"))
		{
			// Stored as LMMS does: the channel's number in the track's settings
			const auto it = std::find(m_channels.begin(), m_channels.end(), proto::parseId(values.value("channel")));
			QDomElement settings = trackSettings(track);
			if (it == m_channels.end() || settings.isNull()) { return false; }
			settings.setAttribute("mixch", static_cast<int>(it - m_channels.begin()));
			values.remove("channel");
		}
		setFields(track, values, proto::trackFields());
		if (values.contains("muted")) { track.setAttribute("mutedBeforeSolo", values.value("muted").toBool() ? 1 : 0); }
		return true;
	}
	return false;
}


void ProjectState::permutePatternClips(const std::vector<Id>& oldPatternOrder)
{
	std::vector<Id> newOrder;
	for (const QDomElement& p : patternTracks()) { newOrder.push_back(idOf(p)); }
	if (newOrder == oldPatternOrder) { return; }

	// Pattern N's content is clip N of every Pattern Editor track: it moves with its pattern track
	for (QDomElement track : patternEditorTracks())
	{
		const auto clips = childClips(track);
		if (clips.size() != oldPatternOrder.size()) { continue; } // inconsistent, leave it alone
		std::vector<QString> slotPositions;
		for (const auto& c : clips) { slotPositions.push_back(c.attribute("pos")); }
		for (std::size_t newSlot = 0; newSlot < newOrder.size(); ++newSlot)
		{
			const auto oldSlot = static_cast<std::size_t>(
				std::find(oldPatternOrder.begin(), oldPatternOrder.end(), newOrder[newSlot]) - oldPatternOrder.begin());
			QDomElement clip = clips[oldSlot];
			clip.setAttribute("pos", slotPositions[newSlot]);
			track.appendChild(clip);
		}
	}
}


bool ProjectState::applyClipOp(const QString& type, const QJsonObject& op)
{
	if (type == proto::op::ClipAdd)
	{
		// Only in the Song Editor: Pattern Editor clips come and go with patterns and tracks
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
	if (!m_clips.contains(clipId)) { return false; }
	const Id trackId = m_clipTrack.value(clipId);
	const bool inPatternEditor = m_patternEditorTracks.contains(trackId);
	if (!m_songTracks.contains(trackId) && !inPatternEditor) { return false; }
	QDomElement clip = m_clips.value(clipId);

	if (type == proto::op::ClipRemove)
	{
		if (inPatternEditor) { return false; }
		m_tracks.value(trackId).removeChild(clip);
		unindexClip(clipId);
		return true;
	}

	if (type == proto::op::ClipSet)
	{
		const QJsonObject values = op.value("v").toObject();
		if (values.isEmpty() || !proto::validFields(values, proto::clipFields())) { return false; }
		if (values.contains("steps") && clip.tagName() != "midiclip") { return false; }
		if (values.contains("src"))
		{
			if (clip.tagName() != "sampleclip") { return false; }
			clip.removeAttribute("data"); // the embedded sample it had before, if any
		}
		if (values.contains("pos") && inPatternEditor) { return false; } // the position is the pattern
		setFields(clip, values, proto::clipFields());
		if (clip.tagName() == "automationclip" && values.contains("muted"))
		{
			// LMMS calls it "mute" in automation clips
			clip.setAttribute("mute", clip.attribute("muted"));
			clip.removeAttribute("muted");
		}
		return true;
	}
	return false;
}


bool ProjectState::applyPatternOp(const QString& type, const QJsonObject& op)
{
	if (m_songContainer.isNull() || m_patternContainer.isNull()) { return false; }

	if (type == proto::op::PatternAdd)
	{
		QDomElement track = parseFragment(op.value("xml"), "track");
		if (track.isNull() || track.attribute("type").toInt() != PatternTrack || !idsAreNew(track)
			|| !track.elementsByTagName("trackcontainer").isEmpty())
		{
			return false;
		}
		// Exactly one new clip for every Pattern Editor track
		const auto editorTracks = patternEditorTracks();
		std::vector<QDomElement> clips(editorTracks.size());
		const QJsonArray entries = op.value("clips").toArray();
		if (entries.size() != static_cast<qsizetype>(editorTracks.size())) { return false; }
		QSet<Id> clipIds;
		for (const QJsonValue& v : entries)
		{
			const QJsonObject entry = v.toObject();
			const Id trackId = proto::parseId(entry.value("track"));
			const auto it = std::find_if(editorTracks.begin(), editorTracks.end(),
				[trackId](const QDomElement& t) { return idOf(t) == trackId; });
			if (it == editorTracks.end()) { return false; }
			const std::size_t i = static_cast<std::size_t>(it - editorTracks.begin());
			QDomElement clip = parseFragment(entry.value("xml"), clipTagFor(it->attribute("type").toInt()));
			if (clip.isNull() || !clips[i].isNull() || !idsAreNew(clip) || clipIds.contains(idOf(clip))) { return false; }
			clipIds.insert(idOf(clip));
			clips[i] = clip;
		}
		if (clipIds.contains(idOf(track))) { return false; }

		const int newPattern = static_cast<int>(patternTracks().size());
		normalize(track);
		QDomElement imported = m_doc.importNode(track, true).toElement();
		m_songContainer.appendChild(imported);
		indexTrack(imported, true);
		for (std::size_t i = 0; i < editorTracks.size(); ++i)
		{
			QDomElement editorTrack = editorTracks[i];
			QDomElement clip = m_doc.importNode(clips[i], true).toElement();
			clip.setAttribute("pos", newPattern * ticksPerBar());
			editorTrack.appendChild(clip);
			indexClip(clip, idOf(editorTrack));
		}
		return true;
	}

	if (type == proto::op::PatternRemove)
	{
		const Id trackId = proto::parseId(op.value("id"));
		if (!m_songTracks.contains(trackId)) { return false; }
		QDomElement track = m_tracks.value(trackId);
		if (track.attribute("type").toInt() != PatternTrack) { return false; }
		const auto patterns = patternTracks();
		const auto rank = static_cast<std::size_t>(
			std::find(patterns.begin(), patterns.end(), track) - patterns.begin());

		// The Pattern Editor's tracks are stored in one of the pattern tracks: keep them in another one
		QDomElement holder = m_patternContainer.parentNode().toElement(); // <patterntrack>
		if (holder.parentNode() == track)
		{
			if (patterns.size() < 2) { return false; } // the last pattern holds the Pattern Editor
			QDomElement other = patterns[rank == 0 ? 1 : 0];
			QDomElement otherHolder = other.firstChildElement("patterntrack");
			if (otherHolder.isNull())
			{
				otherHolder = m_doc.createElement("patterntrack");
				other.appendChild(otherHolder);
			}
			otherHolder.appendChild(m_patternContainer); // moves it
		}

		// Remove clip N of every Pattern Editor track; later patterns move one bar to the left
		for (QDomElement editorTrack : patternEditorTracks())
		{
			const auto clips = childClips(editorTrack);
			if (rank >= clips.size()) { continue; }
			for (std::size_t i = clips.size() - 1; i > rank; --i)
			{
				QDomElement c = clips[i];
				c.setAttribute("pos", clips[i - 1].attribute("pos"));
			}
			unindexClip(idOf(clips[rank]));
			editorTrack.removeChild(clips[rank]);
		}
		removeTrackElement(trackId);
		return true;
	}
	return false;
}


bool ProjectState::applyNotesOp(const QJsonObject& op)
{
	const QJsonValue text = op.value("text");
	if (!text.isString() || text.toString().size() > proto::MaxNotesSize) { return false; }
	QDomElement song = m_doc.documentElement().firstChildElement("song");
	QDomElement notes = song.firstChildElement("projectnotes");
	if (notes.isNull())
	{
		notes = m_doc.createElement("projectnotes");
		song.appendChild(notes);
	}
	while (notes.hasChildNodes()) { notes.removeChild(notes.firstChild()); }
	notes.appendChild(m_doc.createCDATASection(text.toString()));
	return true;
}


bool ProjectState::applyMixerOp(const QString& type, const QJsonObject& op)
{
	const Id master = m_channels.front();

	if (type == proto::op::MixerState)
	{
		QDomDocument fragment;
		if (!op.value("xml").isString() || !fragment.setContent(op.value("xml").toString())) { return false; }
		QDomElement mixer = fragment.documentElement();
		if (mixer.tagName() != "mixer") { return false; }
		// Channels numbered 0..n-1 in order, each with its own id
		QSet<Id> ids;
		int count = 0;
		for (QDomElement c = mixer.firstChildElement("mixerchannel"); !c.isNull(); c = c.nextSiblingElement("mixerchannel"))
		{
			const Id id = idOf(c);
			if (c.attribute("num").toInt() != count++ || id == 0 || ids.contains(id)) { return false; }
			ids.insert(id);
		}
		if (count == 0 || count > 10000) { return false; }
		normalize(mixer);

		// Tracks refer to channels by number: they follow their channel to its new number
		const std::vector<Id> oldChannels = m_channels;
		QDomElement imported = m_doc.importNode(mixer, true).toElement();
		if (m_mixer.isNull()) { m_doc.documentElement().firstChildElement("song").appendChild(imported); }
		else { m_mixer.parentNode().replaceChild(imported, m_mixer); }
		indexMixer();
		for (const QDomElement& track : m_tracks)
		{
			QDomElement settings = trackSettings(track);
			if (settings.isNull() || !settings.hasAttribute("mixch")) { continue; }
			const int old = settings.attribute("mixch").toInt();
			const Id id = old >= 0 && static_cast<std::size_t>(old) < oldChannels.size() ? oldChannels[old] : 0;
			const auto it = std::find(m_channels.begin(), m_channels.end(), id);
			settings.setAttribute("mixch", it == m_channels.end() ? 0 : static_cast<int>(it - m_channels.begin()));
		}
		return true;
	}

	if (type == proto::op::MixerAdd)
	{
		const Id id = proto::parseId(op.value("id"));
		if (id == 0 || isChannel(id) || op.value("index").toInt(-1) < 1) { return false; }
		m_announcedChannels.insert(id); // stored by the mixer.state that follows
		return true;
	}
	if (type == proto::op::MixerRemove)
	{
		const Id id = proto::parseId(op.value("id"));
		return isChannel(id) && id != master;
	}
	if (type == proto::op::MixerOrder)
	{
		QSet<Id> listed;
		for (const QJsonValue& v : op.value("ids").toArray())
		{
			const Id id = proto::parseId(v);
			if (!isChannel(id) || id == master || listed.contains(id)) { return false; }
			listed.insert(id);
		}
		return !listed.isEmpty();
	}
	if (type == proto::op::MixerSet)
	{
		const QJsonObject values = op.value("v").toObject();
		return isChannel(proto::parseId(op.value("id"))) && !values.isEmpty()
			&& proto::validFields(values, proto::mixerChannelFields());
	}
	if (type == proto::op::MixerSend)
	{
		const Id from = proto::parseId(op.value("from"));
		const Id to = proto::parseId(op.value("to"));
		return isChannel(from) && isChannel(to) && from != to && from != master && op.value("on").isBool();
	}
	return false;
}


bool ProjectState::applyAutomationOp(const QJsonObject& op)
{
	// The whole content of an automation clip, written as LMMS saves it (automated parameters by owner
	// and path: journal ids mean nothing across clients)
	const Id clipId = proto::parseId(op.value("clip"));
	QDomElement clip = m_clips.value(clipId);
	if (clip.isNull() || clip.tagName() != "automationclip" || !proto::validAutomationContent(op)) { return false; }

	for (QDomElement e = clip.firstChildElement(); !e.isNull();)
	{
		const QDomElement next = e.nextSiblingElement();
		if (e.tagName() == "time" || e.tagName() == "object") { clip.removeChild(e); }
		e = next;
	}
	auto number = [](const QJsonValue& v) { return QString::number(v.toDouble(), 'g', 9); };
	clip.setAttribute("prog", op.value("prog").toInt());
	clip.setAttribute("tens", number(op.value("tens")));
	for (const QJsonValue& value : op.value("nodes").toArray())
	{
		const QJsonArray node = value.toArray();
		QDomElement time = m_doc.createElement("time");
		time.setAttribute("pos", node[0].toInt());
		time.setAttribute("value", number(node[1]));
		time.setAttribute("outValue", number(node[2]));
		time.setAttribute("inTan", number(node[3]));
		time.setAttribute("outTan", number(node[4]));
		time.setAttribute("lockedTan", node[5].toInt() != 0 ? 1 : 0);
		clip.appendChild(time);
	}
	for (const QJsonValue& value : op.value("objects").toArray())
	{
		QDomElement object = m_doc.createElement("object");
		object.setAttribute("owner", value.toObject().value("owner").toString());
		object.setAttribute("path", value.toObject().value("path").toString());
		clip.appendChild(object);
	}
	return true;
}


bool ProjectState::applyControllerOp(const QString& type, const QJsonObject& op)
{
	const Id id = proto::parseId(op.value("id"));
	if (type == proto::op::ControllersState)
	{
		// The whole Controller Rack; connections refer to controllers by their position in it
		QDomDocument fragment;
		if (!op.value("xml").isString() || !fragment.setContent(op.value("xml").toString())) { return false; }
		const QDomElement controllers = fragment.documentElement();
		if (controllers.tagName() != "controllers" || controllers.childNodes().size() > 1000) { return false; }
		QDomElement song = m_doc.documentElement().firstChildElement("song");
		QDomElement imported = m_doc.importNode(controllers, true).toElement();
		const QDomElement old = song.firstChildElement("controllers");
		if (old.isNull()) { song.appendChild(imported); }
		else { song.replaceChild(imported, old); }
		indexControllers();
		return true;
	}
	if (type == proto::op::ControllerAdd)
	{
		QDomDocument fragment;
		if (id == 0 || isController(id) || !op.value("xml").isString() || !fragment.setContent(op.value("xml").toString())
			|| fragment.documentElement().tagName() != "lfocontroller")
		{
			return false;
		}
		m_announcedControllers.insert(id); // stored by the controllers.state that follows
		return true;
	}
	if (type == proto::op::ControllerRemove) { return isController(id); }
	if (type == proto::op::ControllerSet)
	{
		// An LFO by id, or a Peak Controller by its effect ("ref": "p:<owner>:<n>"); the rack is stored by
		// the controllers.state that comes with it
		const QJsonObject values = op.value("v").toObject();
		static const std::vector<proto::FieldSpec> fields{{"name", proto::FieldSpec::Kind::String}};
		const QString ref = op.value("ref").toString();
		const bool known = ref.isEmpty() ? isController(id)
			: ref.startsWith("p:") && proto::isValidControllerRef(op.value("ref"));
		return known && !values.isEmpty() && proto::validFields(values, fields);
	}
	if (type == proto::op::ParamLink)
	{
		// Relayed; the connection is stored with the settings of the parameter's owner (track.state...)
		const QJsonValue owner = op.value("owner");
		return proto::isValidParamRef(owner, op.value("path")) && proto::isValidControllerRef(op.value("controller"))
			&& (owner.toString() == proto::SongOwner || isParamOwner(proto::parseId(owner)));
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
