/*
 * KeepWindowOrder.h - keeps the order of LMMS's subwindows while windows are deleted
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

#ifndef LMMS_GUI_KEEP_WINDOW_ORDER_H
#define LMMS_GUI_KEEP_WINDOW_ORDER_H

#include <QList>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPointer>
#include <QTimer>

#include "GuiApplication.h"
#include "MainWindow.h"

namespace lmms::gui
{

//! Deleting windows (effect views, an instrument's view) must not bring another window (e.g. the mixer) to
//! the front: the stacking order and the active window are put back as they were
class KeepWindowOrder
{
public:
	KeepWindowOrder()
	{
		auto gui = gui::getGUI();
		if (!gui || !gui->mainWindow()) { return; }
		m_area = gui->mainWindow()->workspace();
		for (QMdiSubWindow* window : m_area->subWindowList(QMdiArea::StackingOrder)) { m_order.append(window); }
		m_active = m_area->activeSubWindow();
	}
	~KeepWindowOrder()
	{
		restore(m_area, m_order, m_active);
		// Qt may also activate another window once deleted widgets are really gone
		QTimer::singleShot(0, [area = m_area, order = m_order, active = m_active] { restore(area, order, active); });
	}

private:
	static void restore(QPointer<QMdiArea> area, const QList<QPointer<QMdiSubWindow>>& order, QPointer<QMdiSubWindow> active)
	{
		if (!area) { return; }
		QList<QMdiSubWindow*> before;
		for (const auto& window : order) { if (window) { before.append(window); } }
		QList<QMdiSubWindow*> now = area->subWindowList(QMdiArea::StackingOrder);
		now.removeIf([&before](QMdiSubWindow* window) { return !before.contains(window); });
		if (now != before)
		{
			for (QMdiSubWindow* window : before) { window->raise(); } // bottom to top
		}
		if (active && area->activeSubWindow() != active) { area->setActiveSubWindow(active); }
	}

	QPointer<QMdiArea> m_area;
	QList<QPointer<QMdiSubWindow>> m_order;
	QPointer<QMdiSubWindow> m_active;
};

} // namespace lmms::gui

#endif // LMMS_GUI_KEEP_WINDOW_ORDER_H
