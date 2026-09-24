/*
 * CollabId.h - persistent identities for collaborative editing
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

#ifndef LMMS_COLLAB_ID_H
#define LMMS_COLLAB_ID_H

#include <cstdint>

#include <QString>

#include "lmms_export.h"

namespace lmms
{

//! Identity of a track, clip or note that stays the same across saves, undo and LMMS instances,
//! so collaborative edits can address objects. 0 means "no id".
using collab_id_t = std::uint64_t;

namespace collab
{

//! XML attribute holding the id, as 16 lowercase hex digits
inline constexpr auto IdAttribute = "cid";

//! Random, non-zero id
LMMS_EXPORT collab_id_t newId();
LMMS_EXPORT QString idToString(collab_id_t id);
//! Returns 0 if @p str is not a valid id
LMMS_EXPORT collab_id_t idFromString(const QString& str);

//! Kinds of objects whose ids must be unique among all live objects of the project.
//! (Note ids only need to be unique within their clip; MidiClip takes care of that.)
enum class IdScope
{
	Track,
	Clip
};

//! Registers @p owner under @p wanted and returns the id actually assigned. If @p wanted is 0 or
//! already belongs to another live object (e.g. a pasted or cloned copy), a fresh id is used.
LMMS_EXPORT collab_id_t claimId(IdScope scope, collab_id_t wanted, const void* owner);
//! Unregisters @p owner from @p id (no-op if the id belongs to someone else)
LMMS_EXPORT void releaseId(IdScope scope, collab_id_t id, const void* owner);
//! Live object registered under @p id, or nullptr
LMMS_EXPORT const void* findOwner(IdScope scope, collab_id_t id);

} // namespace collab

} // namespace lmms

#endif // LMMS_COLLAB_ID_H
