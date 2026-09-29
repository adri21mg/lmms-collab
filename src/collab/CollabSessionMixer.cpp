/*
 * CollabSessionMixer.cpp - synchronization of the mixer (milestone M5a)
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

// Mixer channels are part of the synchronized structure, like tracks: every tick the mixer (channels in
// order, their name, color, shared mute and sends) is compared with the baseline and the differences are
// sent. Channels are identified by ids, never by number: numbers change when channels are removed or moved,
// and tracks refer to their channel by id as well ("channel" in track.set).
//
// Volumes, send amounts and the channels' effects are parameters like those of tracks (the channel's id is
// their owner). The server cannot interpret mixer operations on its XML, so each change comes with the
// whole <mixer> (mixer.state), which the server stores.
//
// Solo is private (as for tracks): LMMS mutes the other channels for a solo, so the shared mute of a channel
// is the one it had before the solo.

#include <algorithm>

#include <QDomDocument>

#include "CollabSession.h"
#include "CollabSessionUtil.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "MixerView.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"

namespace lmms::collab
{

namespace
{

Mixer* mixer()
{
	return Engine::mixer();
}

int channelCount()
{
	return static_cast<int>(mixer()->numChannels());
}

bool anyChannelSolo()
{
	for (int i = 0; i < channelCount(); ++i)
	{
		if (mixer()->mixerChannel(i)->m_soloModel.value()) { return true; }
	}
	return false;
}

//! A solo mutes the other channels (never the master): their shared mute is the one from before the solo
bool sharedChannelMute(int index)
{
	MixerChannel* channel = mixer()->mixerChannel(index);
	return index > 0 && anyChannelSolo() ? channel->m_muteBeforeSolo : channel->m_muteModel.value();
}

QJsonObject channelFields(int index)
{
	MixerChannel* channel = mixer()->mixerChannel(index);
	return {{"name", channel->m_name}, {"color", channel->color() ? channel->color()->name() : QString{}},
		{"muted", sharedChannelMute(index)}};
}

gui::MixerView* mixerView()
{
	auto gui = gui::getGUI();
	return gui ? gui->mixerView() : nullptr;
}

//! Tracks' channel selectors allow every channel that exists (MixerView does this when it has a GUI)
void updateChannelRanges()
{
	for (const TrackContainer* container : {static_cast<TrackContainer*>(Engine::getSong()),
		static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (Track* track : container->tracks())
		{
			IntModel* channel = nullptr;
			if (auto t = dynamic_cast<InstrumentTrack*>(track)) { channel = t->mixerChannelModel(); }
			if (auto t = dynamic_cast<SampleTrack*>(track)) { channel = t->mixerChannelModel(); }
			if (channel) { channel->setRange(0, channelCount() - 1, 1); }
		}
	}
}

//! The n-th automatable model in an object tree, as for tracks (see CollabSessionParams.cpp)
void visitModels(const QObject* object, const QString& prefix, int& n,
	std::vector<std::pair<QString, AutomatableModel*>>& params)
{
	for (const QObject* child : object->children())
	{
		if (auto model = qobject_cast<AutomatableModel*>(const_cast<QObject*>(child)))
		{
			params.emplace_back(QString{"%1:%2"}.arg(prefix).arg(n++), model);
		}
		visitModels(child, prefix, n, params);
	}
}

} // namespace


MixerChannel* findMixerChannel(collab_id_t id)
{
	const int index = mixerChannelIndex(id);
	return index >= 0 ? mixer()->mixerChannel(index) : nullptr;
}


int mixerChannelIndex(collab_id_t id)
{
	if (id == 0) { return -1; }
	for (int i = 0; i < channelCount(); ++i)
	{
		if (mixer()->mixerChannel(i)->collabId() == id) { return i; }
	}
	return -1;
}


void CollabSession::addMixerStructure(Structure& s)
{
	for (int i = 0; i < channelCount(); ++i)
	{
		MixerChannel* channel = mixer()->mixerChannel(i);
		Structure::ChannelInfo info{channelFields(i), {}};
		for (const MixerRoute* send : channel->m_sends) { info.sends.append(send->receiver()->collabId()); }
		s.channels.append(channel->collabId());
		s.channelInfo.insert(channel->collabId(), info);
	}
}


QString CollabSession::mixerXml()
{
	QDomDocument doc;
	QDomElement parent = doc.createElement("collab");
	doc.appendChild(parent);
	mixer()->saveState(doc, parent);
	QDomElement element = parent.firstChildElement();
	for (QDomElement c = element.firstChildElement("mixerchannel"); !c.isNull(); c = c.nextSiblingElement("mixerchannel"))
	{
		const int num = c.attribute("num").toInt();
		c.setAttribute("soloed", 0);
		if (num >= 0 && num < channelCount()) { c.setAttribute("muted", sharedChannelMute(num) ? 1 : 0); }
	}
	return elementToString(element);
}


void CollabSession::flushMixer(const Structure& current, QJsonArray& ops, qint64 ctx)
{
	const Structure& old = m_structure;
	if (current.channels.isEmpty()) { return; }
	const collab_id_t master = current.channels.front();
	const int opsBefore = ops.size();

	for (const collab_id_t id : old.channels)
	{
		if (!current.channelInfo.contains(id)) { ops.append(QJsonObject{{"op", proto::op::MixerRemove}, {"id", proto::idString(id)}}); }
	}
	for (int i = 1; i < current.channels.size(); ++i)
	{
		const collab_id_t id = current.channels[i];
		if (old.channelInfo.contains(id)) { continue; }
		// A new channel is created with default settings everywhere; its own fields follow right away
		ops.append(QJsonObject{{"op", proto::op::MixerAdd}, {"id", proto::idString(id)}, {"index", i}});
		ops.append(QJsonObject{{"op", proto::op::MixerSet}, {"id", proto::idString(id)},
			{"v", current.channelInfo.value(id).fields}});
	}
	for (const collab_id_t id : current.channels)
	{
		if (!old.channelInfo.contains(id)) { continue; }
		const QJsonObject changed = changedFields(old.channelInfo.value(id).fields, current.channelInfo.value(id).fields);
		if (changed.isEmpty()) { continue; }
		ops.append(QJsonObject{{"op", proto::op::MixerSet}, {"id", proto::idString(id)}, {"v", changed}});
		for (auto it = changed.begin(); it != changed.end(); ++it) { m_pendingStructure[pendingKey('m', id, it.key())] = ctx; }
	}
	// Sends (a new channel sends to the master only, as LMMS creates it)
	for (const collab_id_t id : current.channels)
	{
		const QList<collab_id_t> before = old.channelInfo.contains(id) ? old.channelInfo.value(id).sends
			: id == master ? QList<collab_id_t>{} : QList<collab_id_t>{master};
		const QList<collab_id_t>& after = current.channelInfo.value(id).sends;
		auto sendOp = [&](collab_id_t to, bool on) {
			ops.append(QJsonObject{{"op", proto::op::MixerSend}, {"from", proto::idString(id)},
				{"to", proto::idString(to)}, {"on", on}});
		};
		for (const collab_id_t to : after)
		{
			if (!before.contains(to)) { sendOp(to, true); }
		}
		for (const collab_id_t to : before)
		{
			if (!after.contains(to) && current.channelInfo.contains(to)) { sendOp(to, false); }
		}
	}
	// Order: others append new channels, then move them where they are here
	QList<collab_id_t> expected;
	for (const collab_id_t id : old.channels)
	{
		if (current.channelInfo.contains(id)) { expected.append(id); }
	}
	for (const collab_id_t id : current.channels)
	{
		if (!old.channelInfo.contains(id)) { expected.append(id); }
	}
	if (current.channels != expected)
	{
		QJsonArray ids;
		for (int i = 1; i < current.channels.size(); ++i) { ids.append(proto::idString(current.channels[i])); }
		ops.append(QJsonObject{{"op", proto::op::MixerOrder}, {"ids", ids}});
	}

	if (ops.size() != opsBefore)
	{
		ops.append(QJsonObject{{"op", proto::op::MixerState}, {"xml", mixerXml()}});
		m_paramIndexAge.invalidate(); // channels or sends came or went
	}
}


void CollabSession::applyRemoteMixerOp(const QJsonObject& op)
{
	const QString type = op.value("op").toString();
	gui::MixerView* view = mixerView();
	// The same paths as the mixer's own buttons and menus, so its views follow
	auto moveLeft = [&](int index) {
		if (view) { view->moveChannelLeft(index); }
		else { mixer()->moveChannelLeft(index); }
	};

	if (type == proto::op::MixerAdd)
	{
		const collab_id_t id = proto::parseId(op.value("id"));
		if (id == 0 || findMixerChannel(id)) { return; }
		const int index = view ? view->addNewChannel() : mixer()->createChannel();
		mixer()->mixerChannel(index)->setCollabId(id);
	}
	else if (type == proto::op::MixerRemove)
	{
		const int index = mixerChannelIndex(proto::parseId(op.value("id")));
		if (index <= 0) { return; }
		if (view) { view->deleteChannel(index); }
		else
		{
			mixer()->clearChannel(index); // as MixerView does: a soloed channel must not leave others muted
			mixer()->deleteChannel(index);
		}
	}
	else if (type == proto::op::MixerOrder)
	{
		// The listed channels take the positions they occupy now, in the listed order
		std::vector<collab_id_t> wanted;
		std::vector<int> positions;
		for (const QJsonValue& v : op.value("ids").toArray())
		{
			const collab_id_t id = proto::parseId(v);
			const int index = mixerChannelIndex(id);
			if (index <= 0 || std::find(wanted.begin(), wanted.end(), id) != wanted.end()) { continue; }
			wanted.push_back(id);
			positions.push_back(index);
		}
		std::sort(positions.begin(), positions.end());
		for (std::size_t k = 0; k < wanted.size(); ++k)
		{
			while (mixerChannelIndex(wanted[k]) > positions[k]) { moveLeft(mixerChannelIndex(wanted[k])); }
			while (mixerChannelIndex(wanted[k]) < positions[k]) { moveLeft(mixerChannelIndex(wanted[k]) + 1); }
		}
	}
	else if (type == proto::op::MixerSet)
	{
		const collab_id_t id = proto::parseId(op.value("id"));
		const int index = mixerChannelIndex(id);
		if (index < 0) { return; }
		MixerChannel* channel = mixer()->mixerChannel(index);
		QJsonObject fields = op.value("v").toObject();
		for (const QString& key : fields.keys())
		{
			// our unacknowledged write of this field wins
			if (m_pendingStructure.contains(pendingKey('m', id, key))) { fields.remove(key); }
		}
		if (fields.contains("name")) { channel->m_name = fields.value("name").toString(); }
		if (fields.contains("color"))
		{
			const QString color = fields.value("color").toString();
			channel->setColor(color.isEmpty() ? std::nullopt : std::optional<QColor>{QColor{color}});
		}
		if (fields.contains("muted"))
		{
			const bool muted = fields.value("muted").toBool();
			// While this user has a (private) solo, only the mute to restore after the solo changes
			if (index > 0 && anyChannelSolo()) { channel->m_muteBeforeSolo = muted; }
			else { channel->m_muteModel.setValue(muted); }
		}
	}
	else if (type == proto::op::MixerSend)
	{
		const int from = mixerChannelIndex(proto::parseId(op.value("from")));
		const int to = mixerChannelIndex(proto::parseId(op.value("to")));
		if (from <= 0 || to < 0 || from == to) { return; }
		if (op.value("on").toBool())
		{
			if (!mixer()->channelSendModel(from, to) && !mixer()->isInfiniteLoop(from, to)) { mixer()->createChannelSend(from, to); }
		}
		else { mixer()->deleteChannelSend(from, to); }
	}

	updateChannelRanges();
	if (view)
	{
		for (int i = 0; i < channelCount(); ++i) { view->updateMixerChannel(i); }
	}
}


std::vector<std::pair<QString, AutomatableModel*>> CollabSession::enumerateChannelParams(MixerChannel* channel)
{
	std::vector<std::pair<QString, AutomatableModel*>> params;
	params.emplace_back("c:0", &channel->m_volumeModel);
	for (MixerRoute* send : channel->m_sends)
	{
		params.emplace_back("s:" + proto::idString(send->receiver()->collabId()), send->amount());
	}
	int n = 0;
	params.emplace_back(QString{"fx:%1"}.arg(n++), channel->m_fxChain.enabledModel());
	for (Effect* effect : channel->m_fxChain.effects()) { visitModels(effect, "fx", n, params); }
	return params;
}

} // namespace lmms::collab
