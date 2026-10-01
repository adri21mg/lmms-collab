/*
 * CollabLocalGesture.h - whether the user is in the middle of a mouse gesture in this LMMS
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

#ifndef LMMS_COLLAB_LOCAL_GESTURE_H
#define LMMS_COLLAB_LOCAL_GESTURE_H

namespace lmms::collab
{

//! A mouse button is held down in this LMMS right now (remote changes wait until the gesture ends)
bool localGestureInProgress();

} // namespace lmms::collab

#endif // LMMS_COLLAB_LOCAL_GESTURE_H
