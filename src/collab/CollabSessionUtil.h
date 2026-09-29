/*
 * CollabSessionUtil.h - helpers shared by the parts of CollabSession
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

#ifndef LMMS_COLLAB_SESSION_UTIL_H
#define LMMS_COLLAB_SESSION_UTIL_H

#include <QDomElement>
#include <QJsonObject>
#include <QString>
#include <QTextStream>

#include "CollabId.h"
#include "CollabProtocol.h"

namespace lmms::collab
{

inline QString elementToString(const QDomElement& element)
{
	QString text;
	QTextStream stream{&text};
	element.save(stream, 0);
	return text;
}

//! Key of a written field in CollabSession::m_pendingStructure: kind ('t' track, 'c' clip, 'n' notes,
//! 'm' mixer channel), object id, field
inline QString pendingKey(char kind, collab_id_t id, const QString& field)
{
	return QString{"%1:%2:%3"}.arg(kind).arg(proto::idString(id), field);
}

//! Fields of @p after that differ from @p before (fields starting with '_' are never sent)
inline QJsonObject changedFields(const QJsonObject& before, const QJsonObject& after)
{
	QJsonObject changed;
	for (auto it = after.begin(); it != after.end(); ++it)
	{
		if (!it.key().startsWith('_') && before.value(it.key()) != it.value()) { changed.insert(it.key(), it.value()); }
	}
	return changed;
}

} // namespace lmms::collab

#endif // LMMS_COLLAB_SESSION_UTIL_H
