/*
 * CollabLocalGesture.cpp - whether the user is in the middle of a mouse gesture in this LMMS
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

#include "CollabLocalGesture.h"

#include <QCoreApplication>
#include <QGuiApplication>

#include "lmmsconfig.h"

#ifdef LMMS_BUILD_WIN32
#include <windows.h>
#endif

namespace lmms::collab
{

bool localGestureInProgress()
{
	if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) { return false; }
	if (QGuiApplication::mouseButtons() == Qt::NoButton) { return false; }
	// The pressed buttons are only what this program last saw: when another program is the active one,
	// the button was let go over there (e.g. another LMMS on the same computer), so nothing is going on here
	if (QGuiApplication::applicationState() != Qt::ApplicationActive) { return false; }
#ifdef LMMS_BUILD_WIN32
	// What the mouse really does now
	return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
#else
	return true;
#endif
}

} // namespace lmms::collab
