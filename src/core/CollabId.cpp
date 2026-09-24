/*
 * CollabId.cpp - persistent identities for collaborative editing
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

#include "CollabId.h"

#include <array>
#include <mutex>
#include <unordered_map>

#include <QRandomGenerator>

namespace lmms::collab
{

namespace
{

using Registry = std::unordered_map<collab_id_t, const void*>;

std::mutex s_registryMutex;
std::array<Registry, 2> s_registries;

Registry& registry(IdScope scope)
{
	return s_registries[static_cast<std::size_t>(scope)];
}

} // namespace


collab_id_t newId()
{
	collab_id_t id = 0;
	while (id == 0) { id = QRandomGenerator::system()->generate64(); }
	return id;
}


QString idToString(collab_id_t id)
{
	return QString{"%1"}.arg(id, 16, 16, QChar{'0'});
}


collab_id_t idFromString(const QString& str)
{
	bool ok = false;
	const collab_id_t id = str.toULongLong(&ok, 16);
	return ok ? id : 0;
}


collab_id_t claimId(IdScope scope, collab_id_t wanted, const void* owner)
{
	const auto lock = std::lock_guard{s_registryMutex};
	auto& reg = registry(scope);

	if (wanted != 0)
	{
		const auto it = reg.find(wanted);
		if (it == reg.end() || it->second == owner)
		{
			reg[wanted] = owner;
			return wanted;
		}
	}

	collab_id_t id = newId();
	while (reg.find(id) != reg.end()) { id = newId(); }
	reg[id] = owner;
	return id;
}


void releaseId(IdScope scope, collab_id_t id, const void* owner)
{
	const auto lock = std::lock_guard{s_registryMutex};
	auto& reg = registry(scope);
	const auto it = reg.find(id);
	if (it != reg.end() && it->second == owner) { reg.erase(it); }
}


const void* findOwner(IdScope scope, collab_id_t id)
{
	const auto lock = std::lock_guard{s_registryMutex};
	const auto& reg = registry(scope);
	const auto it = reg.find(id);
	return it != reg.end() ? it->second : nullptr;
}

} // namespace lmms::collab
