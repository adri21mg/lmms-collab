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

#include "CollabProtocol.h"

namespace lmms::collab
{

namespace
{

//! Direct children of <song> that only describe one user's editors, never shared (decision D9).
//! Window geometry attributes on other elements are not stripped yet.
const QStringList PrivateSongElements = {"pianoroll", "automationeditor", "ControllerRackView", "timeline"};

using proto::NoteValues;

int noteAttr(const QDomElement& note, NoteValues::Field f)
{
	return note.attribute(NoteValues::FieldNames[f]).toInt();
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
	stripPrivateState();
	index();
	return true;
}


void ProjectState::stripPrivateState()
{
	QDomElement song = m_doc.documentElement().firstChildElement("song");
	for (const QString& name : PrivateSongElements)
	{
		for (QDomElement e = song.firstChildElement(name); !e.isNull(); e = song.firstChildElement(name))
		{
			song.removeChild(e);
		}
	}
}


void ProjectState::index()
{
	m_clips.clear();
	m_notes.clear();
	m_unsortedClips.clear();

	const QDomNodeList clips = m_doc.elementsByTagName("midiclip");
	for (int i = 0; i < clips.size(); ++i)
	{
		QDomElement clip = clips.at(i).toElement();
		const Id clipId = proto::parseId(clip.attribute("cid"));
		if (clipId == 0 || m_clips.contains(clipId)) { continue; } // not addressable
		m_clips.insert(clipId, clip);
		auto& notes = m_notes[clipId];
		for (QDomElement n = clip.firstChildElement("note"); !n.isNull(); n = n.nextSiblingElement("note"))
		{
			const Id noteId = proto::parseId(n.attribute("cid"));
			if (noteId != 0) { notes.insert(noteId, n); }
		}
	}
}


bool ProjectState::apply(const QJsonObject& op)
{
	const QString type = op.value("op").toString();
	const Id clipId = proto::parseId(op.value("clip"));
	const Id noteId = proto::parseId(op.value("id"));
	if (clipId == 0 || noteId == 0 || !m_clips.contains(clipId)) { return false; }

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
