/*
 * CollabSessionParams.cpp - synchronization of knobs, sliders, buttons and song settings (milestone M4)
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

// How parameters are identified: every client builds a track from the same XML with the same code, so
// the models (knobs, sliders...) of a track are created in the same order everywhere. A parameter is the
// n-th AutomatableModel in the track's object tree ("t:<n>") or in its effect chain ("fx:<n>"), which
// works for every instrument and effect without knowing their parameters, and in any UI language.
//
// Local changes are found by comparing values with the last synchronized ones ~20 times per second;
// values driven by automation or controllers (LFOs...) are not sent: every client computes them itself.
// The server cannot map these paths to its XML, so once a track's knobs come to rest, the client that
// made the latest change (by server order, so exactly one) sends the track's settings for it to store.

#include <functional>

#include <QDomDocument>
#include <QFile>
#include <QTextStream>

#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "AudioBusHandle.h"
#include "CollabSession.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MeterModel.h"
#include "Microtuner.h"
#include "MidiPort.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"

namespace lmms::collab
{

namespace
{
constexpr int ParamIndexMaxAgeMs = 1000;  // plugins may create parameters later (e.g. when loading)
constexpr int TrackStateRestMs = 1000;    // how long knobs must rest before the settings are stored

//! Every automatable model driven by automation clips (their values are not the user's doing)
QSet<const AutomatableModel*> automatedModels()
{
	QSet<const AutomatableModel*> automated;
	std::vector<Track*> tracks = Engine::getSong()->tracks();
	const auto& patternTracks = Engine::patternStore()->tracks();
	tracks.insert(tracks.end(), patternTracks.begin(), patternTracks.end());
	tracks.push_back(Engine::getSong()->globalAutomationTrack());
	for (Track* track : tracks)
	{
		if (!track || (track->type() != Track::Type::Automation && track->type() != Track::Type::HiddenAutomation))
		{
			continue;
		}
		for (const Clip* clip : track->getClips())
		{
			// Like AutomationClip::isAutomated(): only clips with nodes automate. LMMS keeps empty global
			// clips for tempo, time signature and master volume/pitch, which must not count.
			auto automation = dynamic_cast<const AutomationClip*>(clip);
			if (automation && automation->hasAutomation())
			{
				for (const auto& object : automation->objects()) { automated.insert(object.data()); }
			}
		}
	}
	return automated;
}

//! Type and range of a parameter: a remote change is only applied to a parameter that looks the same, so a
//! numbering difference (e.g. a plugin that created an extra parameter on one side) never hits the wrong knob
QString fingerprint(const AutomatableModel* model)
{
	return QString{"%1/%2/%3"}.arg(model->metaObject()->className())
		.arg(model->minValue<float>()).arg(model->maxValue<float>());
}

QString settingsXml(Track* track)
{
	QDomDocument doc;
	QDomElement parent = doc.createElement("collab");
	doc.appendChild(parent);
	track->saveState(doc, parent);
	QDomElement settings = parent.firstChildElement().firstChildElement(track->nodeName());
	for (const char* attribute : {"x", "y", "width", "height", "visible", "maximized", "minimized", "tab"})
	{
		settings.removeAttribute(attribute); // instrument window state is private
	}
	QString text;
	QTextStream stream{&text};
	settings.save(stream, 0);
	return text;
}

} // namespace


std::vector<std::pair<QString, AutomatableModel*>> CollabSession::enumerateParams(Track* track)
{
	std::vector<std::pair<QString, AutomatableModel*>> params;
	// Mute is shared through the structure, solo is private, mixer channels come with the mixer (M5),
	// and MIDI port settings describe each user's own MIDI devices
	QSet<const QObject*> excluded{track->getMutedModel(), track->getSoloModel()};
	QSet<const QObject*> skippedTrees;
	EffectChain* effects = nullptr;
	const QObject* instrument = nullptr;
	if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track))
	{
		excluded.insert(instrumentTrack->mixerChannelModel());
		skippedTrees.insert(instrumentTrack->midiPort());
		effects = instrumentTrack->audioBusHandle()->effects();
		// The instrument is numbered on its own ("i:<n>"), so it does not depend on what else the track holds
		instrument = instrumentTrack->instrument();
		skippedTrees.insert(instrument);
	}
	else if (auto sampleTrack = dynamic_cast<SampleTrack*>(track))
	{
		excluded.insert(sampleTrack->mixerChannelModel());
		effects = sampleTrack->audioBusHandle()->effects();
	}

	int n = 0;
	std::function<void(const QObject*, const QString&)> visit = [&](const QObject* object, const QString& prefix) {
		for (const QObject* child : object->children())
		{
			if (skippedTrees.contains(child) || dynamic_cast<const Clip*>(child)) { continue; } // clips: own sync
			if (auto model = qobject_cast<AutomatableModel*>(const_cast<QObject*>(child)))
			{
				const QString path = QString{"%1:%2"}.arg(prefix).arg(n++);
				if (!excluded.contains(model)) { params.emplace_back(path, model); }
			}
			visit(child, prefix);
		}
	};
	visit(track, "t");
	if (instrument)
	{
		n = 0;
		visit(instrument, "i");
	}
	if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track))
	{
		// The microtuner (tuning tab) has no parent object either
		Microtuner* microtuner = instrumentTrack->microtuner();
		n = 0;
		params.emplace_back(QString{"mt:%1"}.arg(n++), microtuner->keyRangeImportModel());
		visit(microtuner, "mt");
	}
	if (effects)
	{
		n = 0;
		params.emplace_back(QString{"fx:%1"}.arg(n++), effects->enabledModel());
		visit(effects, "fx");
	}
	return params;
}


QStringList CollabSession::describeParams(Track* track)
{
	QStringList lines;
	for (const auto& [path, model] : enumerateParams(track))
	{
		lines.append(QString{"%1 %2 %3"}.arg(path, fingerprint(model), model->displayName()));
	}
	return lines;
}


std::vector<std::pair<QString, AutomatableModel*>> CollabSession::songParamModels()
{
	Song* song = Engine::getSong();
	return {{"bpm", &song->tempoModel()}, {"num", &song->getTimeSigModel().numeratorModel()},
		{"den", &song->getTimeSigModel().denominatorModel()}, {"vol", &song->masterVolumeModel()},
		{"pitch", &song->masterPitchModel()}};
}


void CollabSession::refreshParamIndex()
{
	QHash<collab_id_t, qsizetype> previousCounts;
	for (auto it = m_paramIndex.cbegin(); it != m_paramIndex.cend(); ++it) { previousCounts.insert(it.key(), it->byPath.size()); }
	m_paramIndex.clear();
	auto add = [this, &previousCounts](collab_id_t owner, const std::vector<std::pair<QString, AutomatableModel*>>& params) {
		if (m_log && previousCounts.value(owner, -1) != static_cast<qsizetype>(params.size()))
		{
			// Diagnostics: the numbering of this owner (changed); compare with the other client's log
			log(QString{"params of %1: %2"}.arg(owner == 0 ? QString{"song"} : proto::idString(owner)).arg(params.size()));
			for (const auto& [path, model] : params)
			{
				log(QString{"  %1 %2 %3"}.arg(path, fingerprint(model), model->displayName()));
			}
		}
		ParamIndex& index = m_paramIndex[owner];
		for (const auto& [path, model] : params)
		{
			index.byPath[path] = model;
			index.byModel.insert(model, path);
		}
	};
	add(0, songParamModels());
	for (const TrackContainer* container : {static_cast<TrackContainer*>(Engine::getSong()),
		static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (Track* track : container->tracks())
		{
			const bool content = track->type() == Track::Type::Instrument || track->type() == Track::Type::Sample;
			if (content && m_structure.tracks.contains(track->collabId()))
			{
				add(track->collabId(), enumerateParams(track));
			}
		}
	}
	m_paramIndexAge.start();
}


std::optional<CollabSession::ParamKey> CollabSession::paramKeyOf(const AutomatableModel* model) const
{
	for (auto it = m_paramIndex.cbegin(); it != m_paramIndex.cend(); ++it)
	{
		if (const auto path = it->byModel.constFind(model); path != it->byModel.cend()) { return ParamKey{it.key(), *path}; }
	}
	return std::nullopt;
}


AutomatableModel* CollabSession::findParam(const ParamKey& key)
{
	auto lookup = [&]() -> AutomatableModel* {
		const auto index = m_paramIndex.constFind(key.first);
		if (index == m_paramIndex.cend()) { return nullptr; }
		const auto it = index->byPath.find(key.second);
		return it != index->byPath.end() ? it->second.data() : nullptr;
	};
	if (AutomatableModel* model = lookup()) { return model; }
	refreshParamIndex(); // new track or parameter
	return lookup();
}


void CollabSession::flushParams()
{
	if (m_state != State::Live) { return; }
	if (!m_paramIndexAge.isValid() || m_paramIndexAge.elapsed() > ParamIndexMaxAgeMs) { refreshParamIndex(); }

	std::optional<QSet<const AutomatableModel*>> automated; // built only when something changed
	const qint64 ctx = m_nextCtx;
	QJsonArray ops;
	for (auto index = m_paramIndex.cbegin(); index != m_paramIndex.cend(); ++index)
	{
		const collab_id_t owner = index.key();
		for (const auto& [path, model] : index->byPath)
		{
			if (!model) { continue; }
			const float value = model->value<float>();
			const ParamKey key{owner, path};
			const auto base = m_paramBaseline.find(key);
			if (base == m_paramBaseline.end())
			{
				m_paramBaseline.insert(key, value); // first time seen: that is the synchronized value
				continue;
			}
			if (*base == value) { continue; }
			const float previous = *base;
			*base = value;

			if (!automated) { automated = automatedModels(); }
			if (automated->contains(model.data()) || model->controllerConnection())
			{
				log(QString{"local %1 %2 = %3 not sent (automated or controlled)"}.arg(proto::idString(owner), path).arg(value));
				continue; // not by hand
			}

			ops.append(QJsonObject{{"op", proto::op::ParamSet},
				{"owner", owner == 0 ? QString{proto::SongOwner} : proto::idString(owner)}, {"path", path}, {"v", value},
				{"k", fingerprint(model)}});
			m_pendingParams[key] = ctx;
			if (owner != 0) { m_ctxParamTracks[ctx].insert(owner); }
			record(Kind::Param, owner, 0, QJsonObject{{"v", previous}}, QJsonObject{{"v", value}}, path);
		}
	}
	if (!ops.isEmpty()) { sendOps(ops); }
}


void CollabSession::paramsAcknowledged(qint64 ctx, qint64 seq)
{
	for (auto it = m_pendingParams.begin(); it != m_pendingParams.end();)
	{
		it = it.value() <= ctx ? m_pendingParams.erase(it) : std::next(it);
	}
	for (auto it = m_ctxParamTracks.begin(); it != m_ctxParamTracks.end();)
	{
		if (it.key() > ctx) { ++it; continue; }
		for (const collab_id_t track : it.value())
		{
			TrackParamState& state = m_trackParams[track];
			if (seq >= state.lastSeq)
			{
				state.lastSeq = seq;
				state.mine = true;
			}
			state.dirty = true;
			state.lastChange.start();
		}
		it = m_ctxParamTracks.erase(it);
	}
}


void CollabSession::applyRemoteParam(const QJsonObject& op, qint64 seq)
{
	const QString owner = op.value("owner").toString();
	const ParamKey key{owner == proto::SongOwner ? 0 : proto::parseId(op.value("owner")), op.value("path").toString()};
	AutomatableModel* model = findParam(key);
	if (!model || !op.value("v").isDouble())
	{
		log(QString{"remote %1 %2 ignored: no such parameter here"}.arg(op.value("owner").toString(), key.second));
		return;
	}
	if (op.contains("k") && op.value("k").toString() != fingerprint(model))
	{
		log(QString{"remote %1 %2 ignored: it is %3 here but %4 there (%5)"}.arg(op.value("owner").toString(),
			key.second, fingerprint(model), op.value("k").toString(), model->displayName()));
		return;
	}

	// Our unacknowledged write of this parameter is ordered after this one by the server and wins
	if (!m_pendingParams.contains(key))
	{
		m_applyingRemote = true;
		model->setValue(static_cast<float>(op.value("v").toDouble()));
		m_applyingRemote = false;
		m_paramBaseline[key] = model->value<float>();
		log(QString{"remote %1 %2 applied: %3 -> %4 (%5)"}.arg(op.value("owner").toString(), key.second)
			.arg(op.value("v").toDouble()).arg(model->value<float>()).arg(model->displayName()));
	}
	else { log(QString{"remote %1 %2 skipped: our own change is pending"}.arg(op.value("owner").toString(), key.second)); }
	if (key.first != 0)
	{
		TrackParamState& state = m_trackParams[key.first];
		if (seq >= state.lastSeq)
		{
			state.lastSeq = seq;
			state.mine = false;
		}
		state.dirty = true;
		state.lastChange.start();
	}
}


void CollabSession::sendTrackStates()
{
	if (m_state != State::Live) { return; }
	QJsonArray ops;
	for (auto it = m_trackParams.begin(); it != m_trackParams.end(); ++it)
	{
		TrackParamState& state = it.value();
		if (!state.dirty || state.lastChange.elapsed() < TrackStateRestMs) { continue; }
		state.dirty = false;
		if (!state.mine) { continue; } // whoever made the latest change stores the result
		if (Track* track = findTrack(it.key()))
		{
			ops.append(QJsonObject{{"op", proto::op::TrackState}, {"id", proto::idString(it.key())},
				{"xml", settingsXml(track)}});
		}
	}
	if (!ops.isEmpty()) { sendOps(ops); }
}

} // namespace lmms::collab
