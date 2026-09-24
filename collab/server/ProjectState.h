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
	//! Parses a project sent by a client or read from disk. Private UI state (editor windows, loop points)
	//! is removed, see stripPrivateState(). Returns false with @p error set if it is not a usable project.
	bool load(const QByteArray& mmp, QString& error);

	//! Validates and applies one operation. Invalid or stale operations (unknown clip or note, bad values)
	//! are rejected without changing anything; that is how last-write-wins resolves e.g. edit-after-delete.
	bool apply(const QJsonObject& op);

	//! Current state as .mmp XML (notes of every clip sorted the way LMMS expects)
	QByteArray toMmp();

	int clipCount() const { return m_clips.size(); }

private:
	using Id = std::uint64_t;

	void stripPrivateState();
	void index();
	void sortNotes(QDomElement& clip);

	QDomDocument m_doc;
	QHash<Id, QDomElement> m_clips;                  // midiclip elements by clip id
	QHash<Id, QHash<Id, QDomElement>> m_notes;       // note elements by clip id, note id
	QSet<Id> m_unsortedClips;
};

} // namespace lmms::collab

#endif // LMMS_COLLAB_PROJECT_STATE_H
