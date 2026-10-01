/*
 * CollabPresence.h - collaborators' cursors, windows and playback positions
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

#ifndef LMMS_GUI_COLLAB_PRESENCE_H
#define LMMS_GUI_COLLAB_PRESENCE_H

#include <map>
#include <optional>
#include <vector>

#include <QColor>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QHBoxLayout;
class QToolButton;

namespace lmms::gui
{

class MainWindow;
class CursorOverlay;

/**
 * Presence (milestone M3): shows where the other collaborators are, never stored in the project.
 *
 * The local pointer is sampled ~40 times per second (so it works over every window, whether or not the
 * widget below tracks the mouse) and described in shared terms: which window (Song Editor, a Piano Roll
 * clip, an instrument window...) and where in it: time and track or key, a mixer channel, or pixels in
 * fixed-layout windows, so the position is right whatever each user's zoom, scroll and window size.
 *
 * Remote cursors are painted by transparent overlays on top of the windows this user has open too; when
 * a cursor is outside the visible area, an arrow at the edge points to it. Cursors glide between updates.
 */
class CollabPresence : public QObject
{
	Q_OBJECT
public:
	struct User
	{
		QString name;
		QColor color;
		QJsonObject cursor;     //!< empty: not over a shared window
		QJsonObject play;       //!< timeline positions and playback
		QJsonObject view;       //!< e.g. the selected mixer channel
		QString lastWindow;     //!< last window the pointer was over (for "go to")
	};

	//! A playback position line in a window
	struct Marker
	{
		int x;
		QRect area;
		bool playing; //!< playing: solid line; stopped: where Play will start (fainter, with a triangle)
	};

	explicit CollabPresence(MainWindow* mainWindow);
	~CollabPresence() override;

	const std::map<QString, User>& users() const { return m_users; }
	bool showPlayheads() const { return m_showPlayheads; }
	void setShowPlayheads(bool show);
	//! Brings the window the collaborator is working in to the front
	void goTo(const QString& clientId);
	//! Human readable name of a shared window key, e.g. "Piano Roll: Riff (Lead)"
	static QString describeWindow(const QString& window);
	//! Default color for a user name
	static QColor defaultColor(const QString& name);

	// Used by the overlays
	//! Shared key of a window (the widget inside an MDI sub window), empty if it has none
	static QString windowKeyOf(QWidget* content);
	//! Position of a remote cursor in @p content's coordinates
	std::optional<QPoint> locate(QWidget* content, const QJsonObject& cursor) const;
	//! Part of @p content where that cursor can be seen (outside of it: edge arrows)
	QRect visibleRect(QWidget* content, const QJsonObject& cursor) const;
	//! Playback position lines of a remote user that apply to what @p content shows
	std::vector<Marker> markers(QWidget* content, const QJsonObject& play) const;
	//! The mixer channel a remote user has selected, if @p content is the mixer and it is visible
	std::optional<QRect> mixerSelection(QWidget* content, const QJsonObject& view) const;
	//! The tab of an instrument window a remote user is in, if this user shows another tab of it
	std::optional<QRect> tabHint(QWidget* content, const QJsonObject& cursor) const;

signals:
	void usersChanged();

private:
	void onPresence(const QJsonObject& presence);
	void sample();
	void send();
	QJsonObject localCursor() const;
	static QJsonObject localPlay();
	static QJsonObject localView();
	void updateOverlays();

	MainWindow* m_mainWindow;
	std::map<QString, User> m_users;
	std::map<QWidget*, QPointer<CursorOverlay>> m_overlays;
	QTimer m_sampleTimer;
	QTimer m_sendTimer;
	QJsonObject m_presence;
	QJsonObject m_sentPresence;
	bool m_showPlayheads = true;
};


//! The list of collaborators in the menu bar ("● Yeray — Piano Roll: Riff"), click to go there
class CollabPresenceBar : public QWidget
{
	Q_OBJECT
public:
	CollabPresenceBar(CollabPresence* presence, QWidget* parent);

private:
	void rebuild();
	void updateStatus();
	//! Makes the menu bar place this bar again (its size changed)
	void relayout();
	//! Same height as the menu bar's items, so the menu bar never changes its height
	void matchMenuHeight();
	void updateSpinner();

	CollabPresence* m_presence;
	QHBoxLayout* m_layout;
	QToolButton* m_status;
	QTimer* m_spinner;
	int m_spinnerAngle = 0;
};

} // namespace lmms::gui

#endif // LMMS_GUI_COLLAB_PRESENCE_H
