/*
 * CollabPresence.cpp - collaborators' cursors, windows and playback positions
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

#include "CollabPresence.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include "CollabId.h"
#include "AutomationClip.h"
#include "AutomationEditor.h"
#include "CollabSession.h"
#include "FileBrowser.h"
#include "SideBar.h"
#include "Controller.h"
#include "ControllerDialog.h"
#include "ControllerRackView.h"
#include "ControllerView.h"
#include "ConfigManager.h"
#include "ControllerRackView.h"
#include "Effect.h"
#include "EffectControlDialog.h"
#include "EffectControls.h"
#include "EffectRackView.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "InstrumentTrackView.h"
#include "InstrumentTrackWindow.h"
#include "MainWindow.h"
#include "MidiClip.h"
#include "Mixer.h"
#include "MixerChannelView.h"
#include "MixerView.h"
#include "PatternEditor.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PianoRoll.h"
#include "ProjectNotes.h"
#include "Song.h"
#include "SongEditor.h"
#include "TabWidget.h"
#include "TrackContainerView.h"
#include "TrackContentWidget.h"
#include "TrackView.h"

namespace lmms::gui
{

namespace
{

using collab::CollabSession;

constexpr int SampleIntervalMs = 25; // pointer sampling and overlay refresh (~40 per second)
constexpr int SendIntervalMs = 33;   // at most ~30 presence messages per second
constexpr int AnimationIntervalMs = 16;
constexpr double GlideTimeMs = 35;   // how quickly a remote cursor catches up with its latest position

template<class T>
T* ancestor(QWidget* widget)
{
	for (; widget; widget = widget->parentWidget())
	{
		if (auto t = dynamic_cast<T*>(widget)) { return t; }
	}
	return nullptr;
}

//! The widget shown inside the MDI sub window that contains @p widget
QWidget* contentOf(QWidget* widget)
{
	MainWindow* mainWindow = getGUI() ? getGUI()->mainWindow() : nullptr;
	if (!mainWindow) { return nullptr; }
	for (; widget; widget = widget->parentWidget())
	{
		if (widget == mainWindow->toolBar() || widget == mainWindow->sideBar() || widget == mainWindow->sharedFilesBrowser()
			|| qobject_cast<QMdiSubWindow*>(widget->parentWidget()))
		{
			return widget;
		}
	}
	return nullptr;
}

//! "Project files/drums/kick.wav": the names from the top of the "Shared project" tab down to @p item
QString itemPath(const QTreeWidgetItem* item)
{
	QStringList names;
	for (; item; item = item->parent()) { names.prepend(item->text(0)); }
	return names.join('/');
}

//! The item named by @p path in @p tree, or the deepest of its folders that exists (@p complete tells which)
QTreeWidgetItem* itemByPath(QTreeWidget* tree, const QString& path, bool* complete = nullptr)
{
	QTreeWidgetItem* found = nullptr;
	const QStringList names = path.split('/');
	for (int level = 0; level < names.size(); ++level)
	{
		QTreeWidgetItem* next = nullptr;
		const int count = found ? found->childCount() : tree->topLevelItemCount();
		for (int i = 0; i < count && !next; ++i)
		{
			QTreeWidgetItem* candidate = found ? found->child(i) : tree->topLevelItem(i);
			if (candidate->text(0) == names[level]) { next = candidate; }
		}
		if (!next)
		{
			if (complete) { *complete = false; }
			return found;
		}
		found = next;
	}
	if (complete) { *complete = true; }
	return found;
}

//! The deepest item along @p item's folders that can be seen (its folders are expanded)
QTreeWidgetItem* visibleAncestor(QTreeWidgetItem* item)
{
	QTreeWidgetItem* visible = item;
	for (QTreeWidgetItem* it = item; it; it = it->parent())
	{
		if (it->parent() && !it->parent()->isExpanded()) { visible = it->parent(); }
	}
	return visible;
}

//! The window (Controls) of the controller named @p name, if it was ever opened
ControllerDialog* controllerDialogOf(MainWindow* mainWindow, const QString& name)
{
	for (QMdiSubWindow* subWindow : mainWindow->workspace()->subWindowList())
	{
		auto dialog = dynamic_cast<ControllerDialog*>(subWindow->widget());
		if (dialog && collab::controllerName(dynamic_cast<Controller*>(dialog->model())) == name) { return dialog; }
	}
	return nullptr;
}

QString idPart(const QString& key)
{
	return key.section(':', 1);
}

Track* trackById(collab_id_t id)
{
	return const_cast<Track*>(static_cast<const Track*>(collab::findOwner(collab::IdScope::Track, id)));
}

Clip* clipById(collab_id_t id)
{
	return const_cast<Clip*>(static_cast<const Clip*>(collab::findOwner(collab::IdScope::Clip, id)));
}

PatternTrack* currentPatternTrack()
{
	const int current = Engine::patternStore()->currentPattern();
	for (Track* t : Engine::getSong()->tracks())
	{
		auto p = dynamic_cast<PatternTrack*>(t);
		if (p && p->patternIndex() == current) { return p; }
	}
	return nullptr;
}

//! The track editor (Song Editor or Pattern Editor) shown in @p content, if any
TrackContainerView* trackEditorOf(const QWidget* content)
{
	auto gui = getGUI();
	if (content == gui->songEditor()) { return gui->songEditor()->m_editor; }
	if (content == gui->patternEditor()) { return gui->patternEditor()->m_editor; }
	return nullptr;
}

TrackView* viewOf(TrackContainerView* editor, collab_id_t trackId)
{
	for (TrackView* view : editor->trackViews())
	{
		if (view->getTrack()->collabId() == trackId) { return view; }
	}
	return nullptr;
}

int xOfTick(const TrackContainerView* editor, int tick)
{
	return static_cast<int>((tick - editor->currentPosition().getTicks()) * editor->pixelsPerBar() / TimePos::ticksPerBar());
}

//! The part of @p widget that is actually visible (not scrolled away or cut by its window), in content coords
QRect shownRect(QWidget* widget, QWidget* content)
{
	// Clip by every ancestor up to the window: that is what scroll areas and small windows do
	if (widget == content) { return content->rect(); } // (its ancestors are outside the window)
	QRect rect{widget->mapTo(content, QPoint{0, 0}), widget->size()};
	for (QWidget* w = widget->parentWidget(); w && w != content; w = w->parentWidget())
	{
		rect &= QRect{w->mapTo(content, QPoint{0, 0}), w->size()};
	}
	return rect & content->rect();
}

//! Area of a track editor where track contents can be seen, in @p content's coordinates
QRect trackArea(TrackContainerView* editor, QWidget* content)
{
	if (editor->trackViews().isEmpty()) { return {}; }
	// The tracks' scroll area viewport (vertical extent), limited to the content column (horizontal extent)
	TrackView* first = editor->trackViews().front();
	QWidget* viewport = first->parentWidget() ? first->parentWidget()->parentWidget() : nullptr;
	QRect area = viewport ? shownRect(viewport, content) : shownRect(editor, content);
	const TrackContentWidget* tcw = first->getTrackContentWidget();
	const int left = tcw->mapTo(content, QPoint{0, 0}).x();
	return area.intersected(QRect{left, area.top(), tcw->width(), area.height()});
}

MixerView* mixerOf(QWidget* content)
{
	return content == getGUI()->mixerView() ? getGUI()->mixerView() : nullptr;
}

//! The effects shown in the mixer (those of its selected channel)
EffectRackView* shownEffects(MixerView* mixer)
{
	for (EffectRackView* rack : mixer->findChildren<EffectRackView*>())
	{
		if (rack->isVisible()) { return rack; }
	}
	return nullptr;
}

//! Visible part of mixer channel @p index: channels scroll, the master channel does not
QRect channelArea(MixerView* mixer, int index, QWidget* content)
{
	if (index == 0) { return shownRect(mixer, content); }
	QWidget* strip = mixer->channelView(index);
	QWidget* viewport = strip->parentWidget() ? strip->parentWidget()->parentWidget() : nullptr;
	return viewport ? shownRect(viewport, content) : shownRect(mixer, content);
}

void bringToFront(MainWindow* mainWindow, QWidget* content)
{
	if (!content) { return; }
	auto subWindow = qobject_cast<QMdiSubWindow*>(content->parentWidget());
	if (!subWindow) { return; }
	subWindow->show();
	content->show();
	subWindow->raise();
	mainWindow->workspace()->setActiveSubWindow(subWindow);
}

QColor labelTextColor(const QColor& background)
{
	return background.lightnessF() > 0.55 ? QColor{20, 20, 20} : QColor{245, 245, 245};
}

} // namespace


//! Transparent layer over one window, painting the collaborators that are there
class CursorOverlay : public QWidget
{
public:
	CursorOverlay(CollabPresence* presence, QWidget* content) :
		QWidget(content),
		m_presence(presence)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_NoSystemBackground);
		setFocusPolicy(Qt::NoFocus);
		setGeometry(content->rect());
		m_animation.setInterval(AnimationIntervalMs);
		connect(&m_animation, &QTimer::timeout, this, &CursorOverlay::refresh);
		m_clock.start();
		show();
	}

	//! Recomputes what to draw and repaints only the areas that changed. Being transparent, every
	//! repaint also repaints the window below, so full repaints many times per second would waste CPU.
	void refresh()
	{
		std::vector<Item> items = compute();
		if (items == m_items) { return; }
		QRegion dirty;
		for (const Item& item : m_items) { dirty += item.bounds; }
		for (const Item& item : items) { dirty += item.bounds; }
		m_items = std::move(items);
		update(dirty);
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p{this};
		p.setRenderHint(QPainter::Antialiasing);
		for (const Item& item : m_items)
		{
			switch (item.kind)
			{
			case Item::Kind::Selection:
			{
				QColor fill = item.color;
				fill.setAlpha(45);
				QColor border = item.color;
				border.setAlpha(170);
				p.setPen(QPen{border, 2});
				p.setBrush(fill);
				p.drawRoundedRect(QRectF{item.area}.adjusted(1, 1, -1, -1), 3, 3);
				break;
			}
			case Item::Kind::Playhead:
			case Item::Kind::StartMarker: drawMarker(p, item); break;
			case Item::Kind::Cursor: drawCursor(p, item); break;
			case Item::Kind::EdgeArrow: drawEdgeArrow(p, item); break;
			}
		}
	}

private:
	struct Item
	{
		enum class Kind { Selection, StartMarker, Playhead, Cursor, EdgeArrow };
		Kind kind;
		QPoint at;          //!< cursor tip, arrow position or marker x
		QPoint target;      //!< where an edge arrow points to
		QRect area;         //!< visible area / marker extent / selected channel
		QColor color;
		QString name;
		QRect bounds;       //!< everything this item paints
		friend bool operator==(const Item&, const Item&) = default;
	};

	QFont labelFont() const
	{
		QFont f = font();
		f.setBold(true);
		f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1));
		return f;
	}

	QRect labelBox(QPoint at, const QString& name) const
	{
		const QRect text = QFontMetrics{labelFont()}.boundingRect(name);
		QRect box{at, QSize{text.width() + 10, text.height() + 4}};
		// Keep the name inside the window
		if (box.right() > rect().right()) { box.moveRight(rect().right()); }
		if (box.top() < rect().top()) { box.moveTop(rect().top()); }
		return box;
	}

	static QPoint edgeLabelPos(QPoint at, QPoint target)
	{
		return at + QPoint{target.x() > at.x() ? -70 : 12, target.y() > at.y() ? -24 : 10};
	}

	//! Cursors glide towards their latest position instead of jumping between network updates
	QPoint glide(const QString& key, QPoint target, double dt, bool& moving)
	{
		auto it = m_shown.find(key);
		if (it == m_shown.end() || QLineF{it->second, target}.length() > 600)
		{
			m_shown[key] = target; // appeared, or a jump across the window: no slow travel
			return target;
		}
		QPointF& shown = it->second;
		const double k = 1 - std::exp(-dt / GlideTimeMs);
		shown += (QPointF{target} - shown) * k;
		if (QLineF{shown, target}.length() < 0.75) { shown = target; }
		else { moving = true; }
		return shown.toPoint();
	}

	std::vector<Item> compute()
	{
		const double dt = std::clamp(static_cast<double>(m_clock.restart()), 1.0, 100.0);
		bool moving = false;
		std::set<QString> used; // cursors no longer shown here are forgotten below

		std::vector<Item> items;
		QWidget* content = parentWidget();
		for (const auto& [id, user] : m_presence->users())
		{
			if (const auto selection = m_presence->mixerSelection(content, user.view))
			{
				items.push_back({Item::Kind::Selection, {}, {}, *selection, user.color, user.name,
					selection->adjusted(-1, -1, 1, 1)});
			}
			if (const auto tab = m_presence->tabHint(content, user.cursor))
			{
				// The collaborator is in this window, but on another tab: tint that tab
				items.push_back({Item::Kind::Selection, {}, {}, *tab, user.color, user.name, tab->adjusted(-1, -1, 1, 1)});
			}
			if (m_presence->showPlayheads())
			{
				for (const auto& marker : m_presence->markers(content, user.play))
				{
					const QPoint at{marker.x, marker.area.top()};
					items.push_back({marker.playing ? Item::Kind::Playhead : Item::Kind::StartMarker, at, at,
						marker.area, user.color, user.name,
						QRect{marker.x - 7, marker.area.top(), 15, marker.area.height() + 1}});
				}
			}
			if (user.cursor.isEmpty()) { continue; }
			const auto point = m_presence->locate(content, user.cursor);
			if (!point) { continue; }
			const QRect visible = m_presence->visibleRect(content, user.cursor);
			if (visible.contains(*point))
			{
				used.insert(id);
				const QPoint at = glide(id, *point, dt, moving);
				const QRect label = labelBox(at + QPoint{12, -18}, user.name);
				items.push_back({Item::Kind::Cursor, at, at, visible, user.color, user.name,
					(QRect{at - QPoint{3, 3}, QSize{18, 26}} | label).adjusted(-2, -2, 2, 2)});
			}
			else if (visible.width() >= 30 && visible.height() >= 30)
			{
				const QRect inner = visible.adjusted(12, 12, -12, -12);
				const QPoint clamped{std::clamp(point->x(), inner.left(), inner.right()),
					std::clamp(point->y(), inner.top(), inner.bottom())};
				used.insert(id);
				const QPoint at = glide(id, clamped, dt, moving);
				const QRect label = labelBox(edgeLabelPos(at, *point), user.name);
				items.push_back({Item::Kind::EdgeArrow, at, *point, visible, user.color, user.name,
					(QRect{at - QPoint{12, 12}, QSize{25, 25}} | label).adjusted(-2, -2, 2, 2)});
			}
		}
		for (auto it = m_shown.begin(); it != m_shown.end();)
		{
			it = used.count(it->first) ? std::next(it) : m_shown.erase(it);
		}
		// Keep animating only while some cursor is still gliding
		if (moving && !m_animation.isActive()) { m_animation.start(); }
		if (!moving && m_animation.isActive()) { m_animation.stop(); }
		return items;
	}

	void drawLabel(QPainter& p, const QRect& box, const Item& item)
	{
		p.setFont(labelFont());
		p.setPen(Qt::NoPen);
		p.setBrush(item.color);
		p.drawRoundedRect(box, 4, 4);
		p.setPen(labelTextColor(item.color));
		p.drawText(box, Qt::AlignCenter, item.name);
	}

	void drawMarker(QPainter& p, const Item& item)
	{
		// Playing: the other's playhead. Stopped: where their Play starts (fainter, like LMMS' own marker)
		const bool playing = item.kind == Item::Kind::Playhead;
		QColor c = item.color;
		c.setAlpha(playing ? 150 : 110);
		p.setPen(QPen{c, playing ? 2.0 : 1.5, playing ? Qt::SolidLine : Qt::DashLine});
		p.drawLine(item.at.x(), item.area.top(), item.at.x(), item.area.bottom());
		if (!playing)
		{
			const QPolygon triangle{QPoint{item.at.x() - 6, item.area.top()}, QPoint{item.at.x() + 6, item.area.top()},
				QPoint{item.at.x(), item.area.top() + 7}};
			p.setPen(Qt::NoPen);
			p.setBrush(c);
			p.drawPolygon(triangle);
		}
	}

	void drawCursor(QPainter& p, const Item& item)
	{
		// Dark arrow like LMMS' own look, outlined with the collaborator's color; name above right
		QPainterPath arrow;
		arrow.moveTo(0, 0);
		arrow.lineTo(0, 17);
		arrow.lineTo(4.5, 13);
		arrow.lineTo(7.5, 20);
		arrow.lineTo(10.5, 18.7);
		arrow.lineTo(7.5, 12);
		arrow.lineTo(13, 12);
		arrow.closeSubpath();
		p.save();
		p.translate(item.at);
		p.setPen(QPen{item.color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
		p.setBrush(QColor{30, 32, 36});
		p.drawPath(arrow);
		p.restore();
		drawLabel(p, labelBox(item.at + QPoint{12, -18}, item.name), item);
	}

	void drawEdgeArrow(QPainter& p, const Item& item)
	{
		// The collaborator is in this window but outside the visible part: point to them
		const double angle = std::atan2(item.target.y() - item.at.y(), item.target.x() - item.at.x());
		const QPolygonF triangle{QPointF{10, 0}, QPointF{-6, -7}, QPointF{-6, 7}};
		p.save();
		p.translate(item.at);
		p.rotate(angle * 180 / std::numbers::pi);
		p.setPen(QPen{QColor{30, 32, 36}, 1});
		p.setBrush(item.color);
		p.drawPolygon(triangle);
		p.restore();
		drawLabel(p, labelBox(edgeLabelPos(item.at, item.target), item.name), item);
	}

	CollabPresence* m_presence;
	std::vector<Item> m_items;
	std::map<QString, QPointF> m_shown; //!< where each cursor is drawn now (gliding towards its target)
	QTimer m_animation;
	QElapsedTimer m_clock;
};


CollabPresence::CollabPresence(MainWindow* mainWindow) :
	QObject(mainWindow),
	m_mainWindow(mainWindow),
	m_showPlayheads(ConfigManager::inst()->value("collab", "showplayheads", "1").toInt() != 0)
{
	auto session = CollabSession::instance();
	connect(session, &CollabSession::presenceReceived, this, &CollabPresence::onPresence);
	connect(session, &CollabSession::presenceCleared, this, [this] {
		m_users.clear();
		updateOverlays();
		emit usersChanged();
	});
	connect(session, &CollabSession::stateChanged, this, [this] {
		// Announce ourselves as soon as we are in (forces the next send)
		m_sentPresence = QJsonObject{{"_", true}};
	});

	m_sampleTimer.setInterval(SampleIntervalMs);
	connect(&m_sampleTimer, &QTimer::timeout, this, &CollabPresence::sample);
	m_sampleTimer.start();
	m_sendTimer.setSingleShot(true);
	connect(&m_sendTimer, &QTimer::timeout, this, &CollabPresence::send);
}


CollabPresence::~CollabPresence()
{
	for (auto& [content, overlay] : m_overlays) { delete overlay.data(); }
}


void CollabPresence::setShowPlayheads(bool show)
{
	m_showPlayheads = show;
	ConfigManager::inst()->setValue("collab", "showplayheads", show ? "1" : "0");
	updateOverlays();
}


QColor CollabPresence::defaultColor(const QString& name)
{
	static const QColor palette[] = {QColor{"#ff6b6b"}, QColor{"#4dabf7"}, QColor{"#51cf66"}, QColor{"#fcc419"},
		QColor{"#cc5de8"}, QColor{"#ff922b"}, QColor{"#22b8cf"}, QColor{"#f06595"}};
	return palette[qHash(name.trimmed().toLower()) % std::size(palette)];
}


// ------------------------------------------------------------------------------------------------
// Local presence

void CollabPresence::sample()
{
	if (CollabSession::instance()->state() != CollabSession::State::Live) { return; }
	const QJsonObject presence{{"cursor", localCursor()}, {"play", localPlay()}, {"view", localView()}};
	if (presence != m_presence || m_sentPresence.contains("_"))
	{
		m_presence = presence;
		if (!m_sendTimer.isActive()) { m_sendTimer.start(SendIntervalMs); }
	}
	if (!m_users.empty()) { updateOverlays(); } // follows local scrolling and zooming too
}


void CollabPresence::send()
{
	if (m_presence == m_sentPresence) { return; }
	CollabSession::instance()->sendPresence(m_presence);
	m_sentPresence = m_presence;
}


QString CollabPresence::windowKeyOf(QWidget* content)
{
	auto gui = getGUI();
	if (!content || !gui) { return {}; }
	auto key = [](const char* kind, collab_id_t id) { return QString{"%1:%2"}.arg(kind, collab::idToString(id)); };
	if (content == gui->songEditor()) { return "song"; }
	if (gui->mainWindow() && content == gui->mainWindow()->toolBar()) { return "toolbar"; }
	if (gui->mainWindow() && content == gui->mainWindow()->sideBar()) { return "sidebar"; }
	if (gui->mainWindow() && content == gui->mainWindow()->sharedFilesBrowser()) { return "files"; }
	if (auto dialog = dynamic_cast<ControllerDialog*>(content))
	{
		// The Controls window of an LFO (Peak Controllers are edited in their effect's window)
		const QString name = collab::controllerName(dynamic_cast<Controller*>(dialog->model()));
		return name.size() == 16 ? "controller:" + name : QString{};
	}
	if (content == gui->patternEditor())
	{
		const PatternTrack* pattern = currentPatternTrack();
		return pattern ? key("pattern", pattern->collabId()) : QString{};
	}
	if (content == gui->pianoRoll())
	{
		const MidiClip* clip = gui->pianoRoll()->currentMidiClip();
		return clip ? key("pianoroll", clip->collabId()) : QString{};
	}
	if (content == gui->automationEditor())
	{
		const AutomationClip* clip = gui->automationEditor()->currentClip();
		return clip ? key("automation", clip->collabId()) : QString{};
	}
	if (auto window = dynamic_cast<InstrumentTrackWindow*>(content)) { return key("instrument", window->model()->collabId()); }
	if (auto dialog = dynamic_cast<EffectControlDialog*>(content))
	{
		auto controls = dynamic_cast<EffectControls*>(dialog->model());
		const Effect* effect = controls ? controls->effect() : nullptr;
		// Effects of a track or of a mixer channel
		const collab_id_t owner = effect ? collab::effectOwner(effect) : 0;
		if (owner) { return QString{"%1:%2"}.arg(key("effect", owner)).arg(collab::effectIndex(effect)); }
		return {};
	}
	if (content == gui->mixerView()) { return "mixer"; }
	if (content == gui->getProjectNotes()) { return "notes"; }
	if (content == gui->getControllerRackView()) { return "controllers"; }
	return {};
}


QJsonObject CollabPresence::localCursor() const
{
	const QPoint global = QCursor::pos();
	QWidget* widget = QApplication::widgetAt(global); // only this instance's windows
	QWidget* content = contentOf(widget);
	const QString window = windowKeyOf(content);
	if (window.isEmpty()) { return {}; }

	QJsonObject cursor{{"w", window}};
	auto pixels = [&](QWidget* relativeTo) {
		const QPoint p = relativeTo->mapFromGlobal(global);
		cursor.insert("x", p.x());
		cursor.insert("y", p.y());
	};

	if (TrackContainerView* editor = trackEditorOf(content))
	{
		if (auto tcw = ancestor<TrackContentWidget>(widget))
		{
			TrackView* view = ancestor<TrackView>(tcw);
			const QPoint p = tcw->mapFromGlobal(global);
			cursor.insert("a", "track:" + collab::idToString(view->getTrack()->collabId()));
			// Same as TrackContentWidget::getPosition()
			cursor.insert("tick", editor->currentPosition().getTicks()
				+ p.x() * TimePos::ticksPerBar() / std::max(1, static_cast<int>(editor->pixelsPerBar())));
			cursor.insert("y", std::clamp(p.y() / static_cast<double>(std::max(1, tcw->height())), 0.0, 1.0));
			return cursor;
		}
		if (auto view = ancestor<TrackView>(widget))
		{
			cursor.insert("a", "head:" + collab::idToString(view->getTrack()->collabId()));
			pixels(view);
			return cursor;
		}
	}
	else if (content == getGUI()->pianoRoll())
	{
		PianoRoll* editor = getGUI()->pianoRoll()->editor();
		const QPoint p = editor->mapFromGlobal(global);
		if (ancestor<PianoRoll>(widget) == editor && editor->noteGridRect().contains(p))
		{
			const auto [tick, key] = editor->tickKeyAt(p);
			cursor.insert("tick", tick);
			cursor.insert("key", std::clamp(key, 0, 200));
			return cursor;
		}
	}
	else if (content == getGUI()->mainWindow()->sharedFilesBrowser())
	{
		// The file or folder under the pointer, by name: everybody has the same tree here
		QTreeWidget* tree = getGUI()->mainWindow()->sharedFilesBrowser()->treeWidget();
		const QPoint p = tree->viewport()->mapFromGlobal(global);
		if (QTreeWidgetItem* item = tree->viewport()->rect().contains(p) ? tree->itemAt(p) : nullptr)
		{
			const QRect rect = tree->visualItemRect(item);
			cursor.insert("a", "item:" + itemPath(item));
			cursor.insert("x", p.x() - rect.x());
			cursor.insert("y", p.y() - rect.y());
			return cursor;
		}
	}
	else if (content == getGUI()->automationEditor())
	{
		// Time and value (as a fraction of the automated range), whatever each user's zoom and scroll
		AutomationEditor* editor = getGUI()->automationEditor()->m_editor;
		const QPoint p = editor->mapFromGlobal(global);
		if (ancestor<AutomationEditor>(widget) == editor && editor->valueGridRect().contains(p))
		{
			const auto [tick, fraction] = editor->tickFractionAt(p);
			cursor.insert("tick", tick);
			cursor.insert("y", std::clamp(static_cast<double>(fraction), 0.0, 1.0));
			return cursor;
		}
	}
	else if (MixerView* mixer = mixerOf(content))
	{
		// Anchored to the channel (or the effects of the selected channel), so it matches whatever the
		// width of each user's mixer and its scroll position
		if (auto strip = ancestor<MixerChannelView>(widget))
		{
			cursor.insert("a", QString{"chan:%1"}.arg(strip->channelIndex()));
			pixels(strip);
			return cursor;
		}
		if (auto rack = ancestor<EffectRackView>(widget))
		{
			cursor.insert("a", QString{"fx:%1"}.arg(mixer->currentMixerChannel()->channelIndex()));
			pixels(rack);
			return cursor;
		}
	}
	pixels(content);
	if (auto window = dynamic_cast<InstrumentTrackWindow*>(content))
	{
		cursor.insert("tab", window->tabWidgetParent()->activeTab());
	}
	return cursor;
}


QJsonObject CollabPresence::localPlay()
{
	const Song* song = Engine::getSong();
	QJsonObject play{{"song", song->getPlayPos(Song::PlayMode::Song).getTicks()},
		{"pattern", song->getPlayPos(Song::PlayMode::Pattern).getTicks()}, {"playing", song->isPlaying()}};
	if (const PatternTrack* p = currentPatternTrack()) { play.insert("pref", collab::idToString(p->collabId())); }
	if (!song->isPlaying()) { return play; }

	play.insert("tick", song->getPlayPos().getTicks());
	switch (song->playMode())
	{
	case Song::PlayMode::Song: play.insert("mode", "song"); break;
	case Song::PlayMode::Pattern:
		play.insert("mode", "pattern");
		if (play.contains("pref")) { play.insert("ref", play.value("pref")); }
		break;
	case Song::PlayMode::MidiClip:
		play.insert("mode", "clip");
		if (const MidiClip* c = song->midiClipToPlay()) { play.insert("ref", collab::idToString(c->collabId())); }
		break;
	default: break;
	}
	return play;
}


QJsonObject CollabPresence::localView()
{
	QJsonObject view;
	if (SideBar* sideBar = getGUI()->mainWindow()->sideBar(); sideBar && sideBar->activeTab() >= 0)
	{
		view.insert("side", sideBar->activeTab());
	}
	MixerView* mixer = getGUI()->mixerView();
	if (mixer && mixer->currentMixerChannel()) { view.insert("mixsel", mixer->currentMixerChannel()->channelIndex()); }
	return view;
}


// ------------------------------------------------------------------------------------------------
// Remote presence

void CollabPresence::onPresence(const QJsonObject& presence)
{
	const QString id = presence.value("clientId").toString();
	if (presence.value("gone").toBool())
	{
		m_users.erase(id);
	}
	else
	{
		User& user = m_users[id];
		user.name = presence.value("user").toString();
		user.color = QColor{presence.value("color").toString()};
		if (!user.color.isValid()) { user.color = defaultColor(user.name); }
		user.cursor = presence.value("cursor").toObject();
		user.play = presence.value("play").toObject();
		user.view = presence.value("view").toObject();
		if (!user.cursor.isEmpty()) { user.lastWindow = user.cursor.value("w").toString(); }
	}
	updateOverlays();
	emit usersChanged();
}


std::optional<QPoint> CollabPresence::locate(QWidget* content, const QJsonObject& cursor) const
{
	if (cursor.value("w").toString() != windowKeyOf(content)) { return std::nullopt; }
	const QString anchor = cursor.value("a").toString();
	const QPoint pixels{cursor.value("x").toInt(), cursor.value("y").toInt()};

	if (anchor.startsWith("track:") || anchor.startsWith("head:"))
	{
		TrackContainerView* editor = trackEditorOf(content);
		TrackView* view = editor ? viewOf(editor, collab::idFromString(idPart(anchor))) : nullptr;
		if (!view) { return std::nullopt; }
		if (anchor.startsWith("track:"))
		{
			TrackContentWidget* tcw = view->getTrackContentWidget();
			const QPoint local{xOfTick(editor, cursor.value("tick").toInt()),
				static_cast<int>(cursor.value("y").toDouble() * tcw->height())};
			return tcw->mapTo(content, local);
		}
		return view->mapTo(content, pixels);
	}
	if (anchor.startsWith("chan:") || anchor.startsWith("fx:"))
	{
		MixerView* mixer = mixerOf(content);
		const int channel = idPart(anchor).toInt();
		if (!mixer || channel >= Engine::mixer()->numChannels()) { return std::nullopt; }
		if (anchor.startsWith("chan:")) { return mixer->channelView(channel)->mapTo(content, pixels); }
		// Effects are only visible if this user shows the same channel's effects
		EffectRackView* rack = shownEffects(mixer);
		if (!rack || mixer->currentMixerChannel()->channelIndex() != channel) { return std::nullopt; }
		return rack->mapTo(content, pixels);
	}
	if (content == getGUI()->pianoRoll() && cursor.contains("tick"))
	{
		PianoRoll* editor = getGUI()->pianoRoll()->editor();
		return editor->mapTo(content, editor->pointOfTickKey(cursor.value("tick").toInt(), cursor.value("key").toInt()));
	}
	if (anchor.startsWith("item:") && content == getGUI()->mainWindow()->sharedFilesBrowser())
	{
		// Only where this user sees the same item (its folders expanded); otherwise its folder is outlined
		QTreeWidget* tree = getGUI()->mainWindow()->sharedFilesBrowser()->treeWidget();
		bool complete = false;
		QTreeWidgetItem* item = itemByPath(tree, anchor.mid(5), &complete);
		if (!item || !complete || visibleAncestor(item) != item) { return std::nullopt; }
		const QRect rect = tree->visualItemRect(item);
		return tree->viewport()->mapTo(content, rect.topLeft() + pixels);
	}
	if (content == getGUI()->automationEditor() && cursor.contains("tick"))
	{
		AutomationEditor* editor = getGUI()->automationEditor()->m_editor;
		return editor->mapTo(content,
			editor->pointOfTickFraction(cursor.value("tick").toInt(), static_cast<float>(cursor.value("y").toDouble())));
	}
	if (auto window = dynamic_cast<InstrumentTrackWindow*>(content))
	{
		// Over another tab of the instrument window, the position means nothing here (see tabHint())
		if (cursor.contains("tab") && cursor.value("tab").toInt() != window->tabWidgetParent()->activeTab())
		{
			return std::nullopt;
		}
	}
	return pixels;
}


std::optional<QRect> CollabPresence::tabHint(QWidget* content, const QJsonObject& cursor) const
{
	// Shared files: the item the collaborator points at, or the folder it is in if that one is collapsed here
	const QString anchor = cursor.value("a").toString();
	if (anchor.startsWith("item:") && content == getGUI()->mainWindow()->sharedFilesBrowser()
		&& cursor.value("w").toString() == "files")
	{
		QTreeWidget* tree = getGUI()->mainWindow()->sharedFilesBrowser()->treeWidget();
		QTreeWidgetItem* item = itemByPath(tree, anchor.mid(5));
		if (!item) { return std::nullopt; }
		const QRect rect = tree->visualItemRect(visibleAncestor(item));
		if (rect.isEmpty()) { return std::nullopt; }
		return QRect{tree->viewport()->mapTo(content, rect.topLeft()), rect.size()}.intersected(
			shownRect(tree->viewport(), content));
	}
	auto window = dynamic_cast<InstrumentTrackWindow*>(content);
	if (!window || !cursor.contains("tab") || cursor.value("w").toString() != windowKeyOf(content)) { return std::nullopt; }
	TabWidget* tabs = window->tabWidgetParent();
	const int tab = cursor.value("tab").toInt();
	if (tab == tabs->activeTab()) { return std::nullopt; }
	const QRect rect = tabs->tabRect(tab);
	if (rect.isEmpty()) { return std::nullopt; }
	return QRect{tabs->mapTo(content, rect.topLeft()), rect.size()};
}


QRect CollabPresence::visibleRect(QWidget* content, const QJsonObject& cursor) const
{
	const QString anchor = cursor.value("a").toString();
	if (anchor.startsWith("track:") || anchor.startsWith("head:"))
	{
		if (TrackContainerView* editor = trackEditorOf(content))
		{
			if (anchor.startsWith("track:")) { return trackArea(editor, content); }
			return shownRect(editor, content);
		}
	}
	if (anchor.startsWith("chan:") || anchor.startsWith("fx:"))
	{
		if (MixerView* mixer = mixerOf(content))
		{
			const int channel = idPart(anchor).toInt();
			if (anchor.startsWith("chan:") && channel < Engine::mixer()->numChannels())
			{
				return channelArea(mixer, channel, content);
			}
			if (EffectRackView* rack = shownEffects(mixer)) { return shownRect(rack, content); }
		}
	}
	if (content == getGUI()->pianoRoll() && cursor.contains("tick"))
	{
		PianoRoll* editor = getGUI()->pianoRoll()->editor();
		const QRect grid = editor->noteGridRect();
		return QRect{editor->mapTo(content, grid.topLeft()), grid.size()}.intersected(content->rect());
	}
	if (anchor.startsWith("item:") && content == getGUI()->mainWindow()->sharedFilesBrowser())
	{
		return shownRect(getGUI()->mainWindow()->sharedFilesBrowser()->treeWidget()->viewport(), content);
	}
	if (content == getGUI()->automationEditor() && cursor.contains("tick"))
	{
		AutomationEditor* editor = getGUI()->automationEditor()->m_editor;
		const QRect grid = editor->valueGridRect();
		return QRect{editor->mapTo(content, grid.topLeft()), grid.size()}.intersected(content->rect());
	}
	return content->rect();
}


std::vector<CollabPresence::Marker> CollabPresence::markers(QWidget* content, const QJsonObject& play) const
{
	std::vector<Marker> result;
	if (play.isEmpty()) { return result; }
	const bool playing = play.value("playing").toBool();
	const QString mode = play.value("mode").toString();
	auto gui = getGUI();

	auto inTrackEditor = [&](TrackContainerView* editor, int tick, bool isPlaying) {
		const QRect area = trackArea(editor, content);
		if (area.isEmpty()) { return; }
		const int x = editor->trackViews().front()->getTrackContentWidget()->mapTo(content, QPoint{0, 0}).x()
			+ xOfTick(editor, tick);
		if (x >= area.left() && x <= area.right()) { result.push_back({x, area, isPlaying}); }
	};

	if (content == gui->songEditor())
	{
		// Their Song Editor position: the playhead while they play the song, otherwise where Play starts
		const bool songPlaying = playing && mode == "song";
		inTrackEditor(gui->songEditor()->m_editor, play.value("song").toInt(), songPlaying);
	}
	else if (content == gui->patternEditor() && windowKeyOf(content) == "pattern:" + play.value("pref").toString())
	{
		const bool patternPlaying = playing && mode == "pattern";
		inTrackEditor(gui->patternEditor()->m_editor, play.value("pattern").toInt(), patternPlaying);
	}
	else if (playing && mode == "clip" && content == gui->pianoRoll()
		&& windowKeyOf(content) == "pianoroll:" + play.value("ref").toString())
	{
		PianoRoll* editor = gui->pianoRoll()->editor();
		const QRect grid = editor->noteGridRect();
		const QRect area = QRect{editor->mapTo(content, grid.topLeft()), grid.size()}.intersected(content->rect());
		const int x = editor->mapTo(content, editor->pointOfTickKey(play.value("tick").toInt(), 0)).x();
		if (x >= area.left() && x <= area.right()) { result.push_back({x, area, true}); }
	}
	return result;
}


std::optional<QRect> CollabPresence::mixerSelection(QWidget* content, const QJsonObject& view) const
{
	// The side bar: the tab the collaborator has open
	if (SideBar* sideBar = getGUI()->mainWindow()->sideBar(); content == sideBar && view.contains("side"))
	{
		QToolButton* button = sideBar->tabButton(view.value("side").toInt());
		if (!button) { return std::nullopt; }
		return QRect{button->mapTo(content, QPoint{0, 0}), button->size()};
	}
	MixerView* mixer = mixerOf(content);
	if (!mixer || !view.contains("mixsel")) { return std::nullopt; }
	const int channel = view.value("mixsel").toInt();
	if (channel >= Engine::mixer()->numChannels()) { return std::nullopt; }
	QWidget* strip = mixer->channelView(channel);
	const QRect rect = QRect{strip->mapTo(content, QPoint{0, 0}), strip->size()}.intersected(
		channelArea(mixer, channel, content));
	if (rect.isEmpty()) { return std::nullopt; }
	return rect;
}


void CollabPresence::updateOverlays()
{
	// One overlay per open window that can show collaborators; they only repaint what changed
	std::vector<QWidget*> contents{m_mainWindow->toolBar(), m_mainWindow->sideBar()};
	if (m_mainWindow->sharedFilesBrowser() && m_mainWindow->sharedFilesBrowser()->isVisible())
	{
		contents.push_back(m_mainWindow->sharedFilesBrowser());
	}
	for (QMdiSubWindow* subWindow : m_mainWindow->workspace()->subWindowList())
	{
		if (subWindow->isVisible()) { contents.push_back(subWindow->widget()); }
	}
	for (QWidget* content : contents)
	{
		if (!content || windowKeyOf(content).isEmpty()) { continue; }
		auto& overlay = m_overlays[content];
		if (!overlay) { overlay = new CursorOverlay(this, content); }
		if (overlay->geometry() != content->rect()) { overlay->setGeometry(content->rect()); }
		overlay->raise();
		overlay->refresh();
	}
	for (auto it = m_overlays.begin(); it != m_overlays.end();)
	{
		it = it->second ? std::next(it) : m_overlays.erase(it); // window closed and deleted
	}
}


QString CollabPresence::describeWindow(const QString& window)
{
	const collab_id_t id = collab::idFromString(idPart(window));
	if (window == "song") { return tr("Song Editor"); }
	if (window == "toolbar") { return tr("Main toolbar"); }
	if (window == "sidebar") { return tr("Side bar"); }
	if (window == "files") { return tr("Shared project files"); }
	if (window.startsWith("controller:"))
	{
		const Controller* c = collab::findController(idPart(window));
		return c ? tr("Controller: %1").arg(c->name()) : tr("Controller");
	}
	if (window == "mixer") { return tr("Mixer"); }
	if (window == "notes") { return tr("Project Notes"); }
	if (window == "controllers") { return tr("Controller Rack"); }
	if (window.startsWith("pattern:"))
	{
		const Track* t = trackById(id);
		return t ? tr("Pattern Editor: %1").arg(t->name()) : tr("Pattern Editor");
	}
	if (window.startsWith("automation:"))
	{
		const Clip* c = clipById(id);
		if (!c) { return tr("Automation Editor"); }
		const QString clipName = c->name().isEmpty() ? tr("clip") : c->name();
		return tr("Automation Editor: %1 (%2)").arg(clipName, c->getTrack()->name());
	}
	if (window.startsWith("pianoroll:"))
	{
		const Clip* c = clipById(id);
		if (!c) { return tr("Piano Roll"); }
		const QString clipName = c->name().isEmpty() ? tr("clip") : c->name();
		return tr("Piano Roll: %1 (%2)").arg(clipName, c->getTrack()->name());
	}
	if (window.startsWith("instrument:"))
	{
		const Track* t = trackById(id);
		return t ? tr("Instrument: %1").arg(t->name()) : tr("Instrument");
	}
	if (window.startsWith("effect:"))
	{
		const collab_id_t owner = collab::idFromString(window.section(':', 1, 1));
		const Effect* e = collab::effectAt(owner, window.section(':', 2).toInt());
		return e ? tr("Effect: %1 (%2)").arg(e->displayName(), collab::effectOwnerName(owner)) : tr("Effect");
	}
	return {};
}


void CollabPresence::goTo(const QString& clientId)
{
	const auto it = m_users.find(clientId);
	if (it == m_users.end()) { return; }
	const QString window = it->second.cursor.isEmpty() ? it->second.lastWindow : it->second.cursor.value("w").toString();
	const collab_id_t id = collab::idFromString(idPart(window));
	auto gui = getGUI();

	if (window == "song") { bringToFront(m_mainWindow, gui->songEditor()); }
	else if (window == "files")
	{
		SideBar* sideBar = m_mainWindow->sideBar();
		FileBrowser* files = m_mainWindow->sharedFilesBrowser();
		for (int i = 0; i < sideBar->tabCount(); ++i)
		{
			if (sideBar->tabButton(i)->toolTip() == files->title() && !files->isVisible()) { sideBar->tabButton(i)->click(); }
		}
		const QString anchor = it->second.cursor.value("a").toString();
		if (QTreeWidgetItem* item = anchor.startsWith("item:") ? itemByPath(files->treeWidget(), anchor.mid(5)) : nullptr)
		{
			for (QTreeWidgetItem* parent = item->parent(); parent; parent = parent->parent()) { parent->setExpanded(true); }
			files->treeWidget()->scrollToItem(item);
		}
	}
	else if (window.startsWith("controller:"))
	{
		// Its Controls window, opened like its button in the Controller Rack does
		ControllerDialog* dialog = controllerDialogOf(m_mainWindow, idPart(window));
		if (!dialog || !dialog->isVisible())
		{
			for (ControllerView* view : gui->getControllerRackView()->findChildren<ControllerView*>())
			{
				if (collab::controllerName(view->getController()) == idPart(window)) { view->editControls(); }
			}
			dialog = controllerDialogOf(m_mainWindow, idPart(window));
		}
		if (dialog) { bringToFront(m_mainWindow, dialog); }
		else { bringToFront(m_mainWindow, gui->getControllerRackView()); }
	}
	else if (window == "mixer") { bringToFront(m_mainWindow, gui->mixerView()); }
	else if (window == "notes") { bringToFront(m_mainWindow, gui->getProjectNotes()); }
	else if (window == "controllers") { bringToFront(m_mainWindow, gui->getControllerRackView()); }
	else if (window.startsWith("pattern:"))
	{
		if (auto pattern = dynamic_cast<PatternTrack*>(trackById(id)))
		{
			Engine::patternStore()->setCurrentPattern(pattern->patternIndex());
			bringToFront(m_mainWindow, gui->patternEditor());
		}
	}
	else if (window.startsWith("pianoroll:"))
	{
		if (auto clip = dynamic_cast<MidiClip*>(clipById(id)))
		{
			gui->pianoRoll()->setCurrentMidiClip(clip);
			bringToFront(m_mainWindow, gui->pianoRoll());
		}
	}
	else if (window.startsWith("automation:"))
	{
		if (auto clip = dynamic_cast<AutomationClip*>(clipById(id)))
		{
			gui->automationEditor()->setCurrentClip(clip);
			bringToFront(m_mainWindow, gui->automationEditor());
		}
	}
	else if (window.startsWith("effect:"))
	{
		// The effect's window if it was opened before; otherwise its track's instrument window or the mixer
		const collab_id_t owner = collab::idFromString(window.section(':', 1, 1));
		const Effect* effect = collab::effectAt(owner, window.section(':', 2).toInt());
		for (QMdiSubWindow* subWindow : m_mainWindow->workspace()->subWindowList())
		{
			auto dialog = dynamic_cast<EffectControlDialog*>(subWindow->widget());
			auto controls = dialog ? dynamic_cast<EffectControls*>(dialog->model()) : nullptr;
			if (effect && controls && controls->effect() == effect)
			{
				bringToFront(m_mainWindow, dialog);
				return;
			}
		}
		if (const int channel = collab::mixerChannelIndex(owner); channel >= 0)
		{
			gui->mixerView()->setCurrentMixerChannel(channel);
			bringToFront(m_mainWindow, gui->mixerView());
			return;
		}
		const QString trackWindow = "instrument:" + window.section(':', 1, 1);
		it->second.lastWindow = trackWindow;
		it->second.cursor = QJsonObject{};
		goTo(clientId);
	}
	else if (window.startsWith("instrument:"))
	{
		for (TrackContainerView* editor : {static_cast<TrackContainerView*>(gui->songEditor()->m_editor),
			static_cast<TrackContainerView*>(gui->patternEditor()->m_editor)})
		{
			if (auto view = dynamic_cast<InstrumentTrackView*>(viewOf(editor, id)))
			{
				// The same as pressing the track's instrument button (a private slot)
				QMetaObject::invokeMethod(view, "toggleInstrumentWindow", Q_ARG(bool, true));
				bringToFront(m_mainWindow, view->getInstrumentTrackWindow());
				break;
			}
		}
	}
}


// ------------------------------------------------------------------------------------------------

CollabPresenceBar::CollabPresenceBar(CollabPresence* presence, QWidget* parent) :
	QWidget(parent),
	m_presence(presence),
	m_layout(new QHBoxLayout(this))
{
	m_layout->setContentsMargins(0, 0, 6, 0);
	m_layout->setSpacing(10);
	connect(presence, &CollabPresence::usersChanged, this, &CollabPresenceBar::rebuild);
	rebuild();
}


void CollabPresenceBar::rebuild()
{
	const auto& users = m_presence->users();
	// One button per collaborator; texts are updated in place (this runs up to 30 times per second)
	auto buttons = findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly);
	while (buttons.size() > static_cast<qsizetype>(users.size())) { delete buttons.takeLast(); }
	while (buttons.size() < static_cast<qsizetype>(users.size()))
	{
		auto button = new QToolButton(this);
		button->setAutoRaise(true);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setToolTip(tr("Click to go where this collaborator is"));
		connect(button, &QToolButton::clicked, this, [this, button] {
			m_presence->goTo(button->property("clientId").toString());
		});
		m_layout->addWidget(button);
		buttons.append(button);
	}
	qsizetype i = 0;
	for (const auto& [id, user] : users)
	{
		QToolButton* button = buttons[i++];
		const QString where = CollabPresence::describeWindow(user.cursor.isEmpty() ? user.lastWindow
			: user.cursor.value("w").toString());
		const QString text = where.isEmpty() ? user.name : user.name + QString::fromUtf8(" — ") + where;
		if (button->text() != text) { button->setText(text); }
		if (button->property("color").value<QColor>() != user.color)
		{
			QPixmap dot{10, 10};
			dot.fill(Qt::transparent);
			QPainter p{&dot};
			p.setRenderHint(QPainter::Antialiasing);
			p.setBrush(user.color);
			p.setPen(Qt::NoPen);
			p.drawEllipse(0, 0, 10, 10);
			button->setIcon(QIcon{dot});
			button->setProperty("color", user.color);
		}
		button->setProperty("clientId", id);
	}
	adjustSize();
}

} // namespace lmms::gui
