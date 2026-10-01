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

#include <cmath>
#include <functional>

#include <QDomDocument>
#include <QFile>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QTextStream>

#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "AudioBusHandle.h"
#include "CollabSession.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "DummyEffect.h"
#include "Effect.h"
#include "EffectControlDialog.h"
#include "EffectControls.h"
#include "EffectRackView.h"
#include "GuiApplication.h"
#include "InstrumentTrackWindow.h"
#include "KeepWindowOrder.h"
#include "MainWindow.h"
#include "MeterModel.h"
#include "Microtuner.h"
#include "MidiPort.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "Mixer.h"
#include "PatternStore.h"
#include "PeakController.h"
#include "PluginFactory.h"
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

QString elementText(const QDomElement& element)
{
	QString text;
	QTextStream stream{&text};
	element.save(stream, 0);
	return text;
}

EffectChain* effectsOf(Track* track)
{
	if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track)) { return instrumentTrack->audioBusHandle()->effects(); }
	if (auto sampleTrack = dynamic_cast<SampleTrack*>(track)) { return sampleTrack->audioBusHandle()->effects(); }
	return nullptr;
}

//! The track's <instrument> element as LMMS saves it (plugin name, its whole state, sub-plugin key)
QString instrumentXml(InstrumentTrack* track)
{
	Instrument* instrument = track->instrument();
	if (!instrument) { return {}; }
	QDomDocument doc;
	QDomElement element = doc.createElement("instrument");
	doc.appendChild(element);
	element.setAttribute("name", instrument->descriptor()->name);
	QDomElement state = instrument->saveState(doc, element);
	if (instrument->key().isValid()) { state.appendChild(instrument->key().saveXML(doc)); }
	return elementText(element);
}

constexpr auto EffectIdProperty = "collabEffectId";

//! Identifies an effect on every client (sent as a "cid" attribute), so that a remote change only touches
//! the effects that changed and keeps the others, their windows and their knobs as they are
QString effectId(Effect* effect)
{
	QString id = effect->property(EffectIdProperty).toString();
	if (id.isEmpty())
	{
		id = idToString(newId());
		effect->setProperty(EffectIdProperty, id);
	}
	return id;
}

QList<QDomElement> effectElements(const QDomElement& chain)
{
	QList<QDomElement> elements;
	const int count = chain.attribute("numofeffects").toInt();
	for (QDomElement e = chain.firstChildElement("effect"); !e.isNull() && elements.size() < count;
		e = e.nextSiblingElement("effect"))
	{
		elements.append(e);
	}
	return elements;
}

QString effectsXml(EffectChain* chain)
{
	QDomDocument doc;
	QDomElement parent = doc.createElement("collab");
	doc.appendChild(parent);
	chain->saveState(doc, parent);
	const QList<QDomElement> elements = effectElements(parent.firstChildElement());
	for (int i = 0; i < elements.size() && i < static_cast<int>(chain->effects().size()); ++i)
	{
		QDomElement element = elements[i];
		element.setAttribute("cid", effectId(chain->effects()[i]));
	}
	return elementText(parent.firstChildElement());
}

//! A number unique to this object for the whole run. Addresses are not enough: a new instrument can be
//! allocated exactly where the deleted one was.
QString serialOf(QObject* object)
{
	static qulonglong s_next = 1;
	if (!object) { return "0"; }
	QVariant serial = object->property("collabSerial");
	if (!serial.isValid())
	{
		serial = s_next++;
		object->setProperty("collabSerial", serial);
	}
	return serial.toString();
}

//! Changes whenever the instrument object is replaced (another plugin, a preset)
QString instrumentIdentity(InstrumentTrack* track)
{
	return serialOf(track->instrument());
}

//! Changes whenever effects are added, removed or moved
QString effectsIdentity(EffectChain* chain)
{
	QStringList ids;
	for (Effect* effect : chain->effects()) { ids.append(serialOf(effect)); }
	return ids.join(',');
}

//! Plugins with a window of their own besides the one LMMS shows (it is not an LMMS subwindow)
bool hasExternalGui(Track* track)
{
	static const QStringList external{"zynaddsubfx", "vestige", "vsteffect", "carlarack", "carlapatchbay"};
	if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
		instrumentTrack && instrumentTrack->instrument()
		&& external.contains(QString::fromUtf8(instrumentTrack->instrument()->descriptor()->name)))
	{
		return true;
	}
	if (EffectChain* chain = effectsOf(track))
	{
		for (Effect* effect : chain->effects())
		{
			if (external.contains(QString::fromUtf8(effect->descriptor()->name))) { return true; }
		}
	}
	return false;
}

QString keyText(const QDomElement& instrument)
{
	const QDomElement key = instrument.elementsByTagName("key").item(0).toElement();
	return key.isNull() ? QString{} : elementText(key);
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
		// In chain order: moving an effect reorders the chain, not the objects (children keep creation order)
		for (Effect* effect : effects->effects()) { visit(effect, "fx"); }
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
	for (int i = 0; i < static_cast<int>(Engine::mixer()->numChannels()); ++i)
	{
		MixerChannel* channel = Engine::mixer()->mixerChannel(i);
		if (m_structure.channelInfo.contains(channel->collabId())) { add(channel->collabId(), enumerateChannelParams(channel)); }
	}
	for (Controller* controller : Engine::getSong()->controllers())
	{
		if (m_structure.controllerFields.contains(controller->collabId()))
		{
			add(controller->collabId(), enumerateControllerParams(controller));
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
			const ParamKey key{owner, path};

			// Which controller it follows
			const QString link = sharedConnection(model);
			const auto linkBase = m_linkBaseline.find(key);
			if (linkBase == m_linkBaseline.end()) { m_linkBaseline.insert(key, link); }
			else if (*linkBase != link && !m_unresolvedLinks.contains(key))
			{
				*linkBase = link;
				ops.append(QJsonObject{{"op", proto::op::ParamLink},
					{"owner", owner == 0 ? QString{proto::SongOwner} : proto::idString(owner)}, {"path", path},
					{"controller", link}});
				m_pendingLinks[key] = ctx;
				if (owner != 0) { m_ctxParamTracks[ctx].insert(owner); } // its owner's settings are stored again
			}

			const float value = model->value<float>();
			const auto base = m_paramBaseline.find(key);
			if (base == m_paramBaseline.end())
			{
				m_paramBaseline.insert(key, value); // first time seen: that is the synchronized value
				continue;
			}
			if (*base == value || (std::isnan(*base) && std::isnan(value))) { continue; }
			const float previous = *base;
			*base = value;
			// e.g. AudioFileProcessor's start/end knobs without a sample: nothing meaningful to send
			if (!std::isfinite(value)) { continue; }

			if (!automated) { automated = automatedModels(); }
			// A MIDI controller is this user's own device: what it does counts as done by hand
			const ControllerConnection* connection = model->controllerConnection();
			const bool midi = connection && const_cast<ControllerConnection*>(connection)->getController()
				&& const_cast<ControllerConnection*>(connection)->getController()->type() == Controller::ControllerType::Midi;
			if (automated->contains(model.data()) || (connection && !midi))
			{
				log(QString{"local %1 %2 = %3 not sent (automated or controlled)"}.arg(proto::idString(owner), path).arg(value));
				continue; // not by hand
			}

			ops.append(QJsonObject{{"op", proto::op::ParamSet},
				{"owner", owner == 0 ? QString{proto::SongOwner} : proto::idString(owner)}, {"path", path}, {"v", value},
				{"k", fingerprint(model)}});
			m_pendingParams[key] = ctx;
			if (owner != 0)
			{
				m_ctxParamTracks[ctx].insert(owner);
				noteParamActivity(owner, true);
			}
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
	for (auto it = m_pendingLinks.begin(); it != m_pendingLinks.end();)
	{
		it = it.value() <= ctx ? m_pendingLinks.erase(it) : std::next(it);
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
		noteParamActivity(key.first, false);
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
	bool mixerState = false;
	bool controllersState = false;
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
		else if (findMixerChannel(it.key())) { mixerState = true; } // the server stores the whole mixer
		else if (findController(proto::idString(it.key()))) { controllersState = true; } // the whole Controller Rack
	}
	if (mixerState) { ops.append(QJsonObject{{"op", proto::op::MixerState}, {"xml", mixerXml()}}); }
	if (controllersState) { ops.append(QJsonObject{{"op", proto::op::ControllersState}, {"xml", controllersXml()}}); }
	if (!ops.isEmpty()) { sendOps(ops); }
}


void CollabSession::resetParamBaselines(collab_id_t owner, const QString& prefix)
{
	for (auto it = m_paramBaseline.begin(); it != m_paramBaseline.end();)
	{
		it = it.key().first == owner && it.key().second.startsWith(prefix) ? m_paramBaseline.erase(it) : std::next(it);
	}
	for (auto it = m_pendingParams.begin(); it != m_pendingParams.end();)
	{
		it = it.key().first == owner && it.key().second.startsWith(prefix) ? m_pendingParams.erase(it) : std::next(it);
	}
	for (auto it = m_linkBaseline.begin(); it != m_linkBaseline.end();)
	{
		it = it.key().first == owner && it.key().second.startsWith(prefix) ? m_linkBaseline.erase(it) : std::next(it);
	}
	m_paramIndexAge.invalidate(); // renumber at the next flush
}


void CollabSession::noteParamActivity(collab_id_t owner, bool local)
{
	PluginState& state = m_plugins[owner];
	(local ? state.localParamsTouched : state.remoteParamsTouched) = true;
	state.lastParamActivity.start();
}


// ------------------------------------------------------------------------------------------------
// Instruments and effect chains (milestone M4b)

void CollabSession::rebasePlugins(Track* track)
{
	PluginState& state = m_plugins[track->collabId()];
	if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track))
	{
		state.instrumentIdentity = instrumentIdentity(instrumentTrack);
		state.instrumentXml = instrumentXml(instrumentTrack);
	}
	if (EffectChain* chain = effectsOf(track)) { rebaseEffects(track->collabId(), chain); }
	state.localParamsTouched = state.remoteParamsTouched = false;
	state.known = true;
}


void CollabSession::rebaseEffects(collab_id_t owner, EffectChain* chain)
{
	PluginState& state = m_plugins[owner];
	state.effectsIdentity = effectsIdentity(chain);
	state.effectsXml = effectsXml(chain);
	state.localParamsTouched = state.remoteParamsTouched = false;
	state.known = true;
}


QSet<collab_id_t> CollabSession::tracksBeingEdited()
{
	QSet<collab_id_t> tracks;
	auto gui = gui::getGUI();
	if (!gui || !gui->mainWindow()) { return tracks; }
	for (QMdiSubWindow* subWindow : gui->mainWindow()->workspace()->subWindowList())
	{
		if (!subWindow->isVisible()) { continue; }
		QWidget* content = subWindow->widget();
		if (auto window = dynamic_cast<gui::InstrumentTrackWindow*>(content)) { tracks.insert(window->model()->collabId()); }
		else if (auto dialog = dynamic_cast<gui::EffectControlDialog*>(content))
		{
			auto controls = dynamic_cast<EffectControls*>(dialog->model());
			if (const collab_id_t owner = controls ? effectOwner(controls->effect()) : 0) { tracks.insert(owner); }
		}
	}
	return tracks;
}


void CollabSession::flushPlugins()
{
	if (m_state != State::Live) { return; }

	// Plugin state that is not made of parameters (a loaded sample, ZynAddSubFX, a VST...) can only be seen by
	// serializing the plugin, so that is done every 2 s for plugins whose windows are open, and once more
	// when their window closes. Replacing an instrument or changing the effects is seen immediately.
	QSet<collab_id_t> checkState;
	if (!m_opaqueCheck.isValid() || m_opaqueCheck.elapsed() >= 2000)
	{
		m_opaqueCheck.start();
		const QSet<collab_id_t> edited = tracksBeingEdited();
		checkState = edited | (m_editedTracks - edited);
		m_editedTracks = edited;
		// ZynAddSubFX, VSTs... have their own window, opened from the track's window, which can stay open when
		// that one closes: once edited, they are checked for the rest of the session
		for (const collab_id_t id : edited)
		{
			if (Track* track = findTrack(id); track && hasExternalGui(track)) { m_externalGuiTracks.insert(id); }
		}
		checkState |= m_externalGuiTracks;
	}

	QJsonArray ops;
	QSet<Track*> changedTracks;
	for (const TrackContainer* container : {static_cast<TrackContainer*>(Engine::getSong()),
		static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (Track* track : container->tracks())
		{
			const collab_id_t id = track->collabId();
			auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track);
			EffectChain* chain = effectsOf(track);
			if (!m_structure.tracks.contains(id) || (!instrumentTrack && !chain)) { continue; }
			PluginState& state = m_plugins[id];
			if (!state.known)
			{
				rebasePlugins(track); // first time seen: that is the synchronized state
				continue;
			}
			// While knobs move, the XML differs because of them; they are synchronized on their own
			const bool quiet = !state.lastParamActivity.isValid() || state.lastParamActivity.elapsed() >= 1000;
			// Also right after one of its files became shared (its instrument now names the shared file)
			const bool compareState = (checkState.contains(id) || m_forcePluginCheck.contains(id)) && quiet;
			if (compareState) { m_forcePluginCheck.remove(id); }
			// The other side's knob changes also change the XML, but they are already synchronized. Our own
			// knob changes are too, yet they may hide a change of state (a sample loaded, a preset...): the
			// whole state is then sent, and the other side only reloads what really differs.
			const bool onlyRemoteKnobs = state.remoteParamsTouched && !state.localParamsTouched;

			if (instrumentTrack)
			{
				const QString identity = instrumentIdentity(instrumentTrack);
				QString xml;
				bool send = identity != state.instrumentIdentity;
				if (!send && compareState)
				{
					xml = instrumentXml(instrumentTrack);
					send = xml != state.instrumentXml && !onlyRemoteKnobs;
					if (!send) { state.instrumentXml = xml; }
				}
				if (send && xml.isEmpty()) { xml = instrumentXml(instrumentTrack); }
				// Files only this computer has are shared first; until then the others keep the old instrument
				if (send && !readyToSend(xml)) { send = false; }
				if (send)
				{
					ops.append(QJsonObject{{"op", proto::op::InstrumentSet}, {"track", proto::idString(id)}, {"xml", xml}});
					state.instrumentIdentity = identity;
					state.instrumentXml = xml;
					resetParamBaselines(id, "i:");
					changedTracks.insert(track);
				}
			}
			if (chain && flushEffects(id, "track", chain, state, compareState, onlyRemoteKnobs)) { changedTracks.insert(track); }
			if (compareState) { state.localParamsTouched = state.remoteParamsTouched = false; }
		}
	}
	// The effects of mixer channels
	bool mixerChanged = false;
	for (int i = 0; i < static_cast<int>(Engine::mixer()->numChannels()); ++i)
	{
		MixerChannel* channel = Engine::mixer()->mixerChannel(i);
		const collab_id_t id = channel->collabId();
		if (!m_structure.channelInfo.contains(id)) { continue; }
		PluginState& state = m_plugins[id];
		if (!state.known)
		{
			rebaseEffects(id, &channel->m_fxChain);
			continue;
		}
		const bool quiet = !state.lastParamActivity.isValid() || state.lastParamActivity.elapsed() >= 1000;
		const bool compareState = checkState.contains(id) && quiet;
		const bool onlyRemoteKnobs = state.remoteParamsTouched && !state.localParamsTouched;
		mixerChanged |= flushEffects(id, "channel", &channel->m_fxChain, state, compareState, onlyRemoteKnobs);
		if (compareState) { state.localParamsTouched = state.remoteParamsTouched = false; }
	}
	// The server stores the result as the track's settings / the mixer
	for (Track* track : changedTracks)
	{
		m_pluginOps.append(QJsonObject{{"op", proto::op::TrackState}, {"id", proto::idString(track->collabId())},
			{"xml", settingsXml(track)}});
	}
	if (mixerChanged) { m_pluginOps.append(QJsonObject{{"op", proto::op::MixerState}, {"xml", mixerXml()}}); }
	for (const QJsonValue& op : m_pluginOps) { ops.append(op); }
	m_pluginOps = QJsonArray{};
	if (!ops.isEmpty()) { sendOps(ops); }
}


bool CollabSession::flushEffects(collab_id_t owner, const char* ownerKey, EffectChain* chain, PluginState& state,
	bool compareState, bool onlyRemoteKnobs)
{
	const QString identity = effectsIdentity(chain);
	QString xml;
	bool send = identity != state.effectsIdentity;
	if (!send && compareState)
	{
		xml = effectsXml(chain);
		send = xml != state.effectsXml && !onlyRemoteKnobs;
		if (!send) { state.effectsXml = xml; }
	}
	if (!send) { return false; }
	if (xml.isEmpty()) { xml = effectsXml(chain); }
	m_pluginOps.append(QJsonObject{{"op", proto::op::EffectsSet}, {ownerKey, proto::idString(owner)}, {"xml", xml}});
	state.effectsIdentity = identity;
	state.effectsXml = xml;
	resetParamBaselines(owner, "fx:");
	for (Effect* effect : chain->effects()) { effect->setProperty("collabEffectIdShared", true); }
	return true;
}


void CollabSession::applyRemoteInstrument(const QJsonObject& op)
{
	auto track = dynamic_cast<InstrumentTrack*>(findTrack(proto::parseId(op.value("track"))));
	QDomDocument doc;
	if (!track || !doc.setContent(op.value("xml").toString()) || doc.documentElement().tagName() != "instrument") { return; }
	const QDomElement element = doc.documentElement();
	const QString name = element.attribute("name");

	const QString currentXml = instrumentXml(track);
	QDomDocument current;
	current.setContent(currentXml);
	const bool samePlugin = track->instrument() && track->instrument()->descriptor()->name == name
		&& keyText(current.documentElement()) == keyText(element);
	if (samePlugin && currentXml == op.value("xml").toString())
	{
		// Typically sent after knob changes that already arrived one by one: nothing to reload
		log(QString{"remote instrument %1 for %2: already the same"}.arg(name, proto::idString(track->collabId())));
		rebasePlugins(track);
		return;
	}

	gui::KeepWindowOrder keepWindowOrder;
	m_applyingRemote = true;
	if (!samePlugin)
	{
		// Like loading a project: another plugin (or sub-plugin, e.g. another VST) for this track
		using PluginKey = Plugin::Descriptor::SubPluginFeatures::Key;
		PluginKey key(element.elementsByTagName("key").item(0).toElement());
		// loadInstrument() renames the track after the plugin: the name is synchronized on its own (a preset
		// dropped on a track renames it)
		const QString trackName = track->name();
		track->loadInstrument(name, &key);
		track->setName(trackName);
	}
	if (track->instrument()) { track->instrument()->restoreState(element.firstChildElement()); }
	m_applyingRemote = false;

	log(QString{"remote instrument %1 for %2 (%3)"}.arg(name, proto::idString(track->collabId()),
		samePlugin ? "state" : "new plugin"));
	resetParamBaselines(track->collabId(), "i:");
	rebasePlugins(track);
}


void CollabSession::applyRemoteEffects(const QJsonObject& op)
{
	// The effects of a track or of a mixer channel
	const collab_id_t owner = proto::parseId(op.contains("channel") ? op.value("channel") : op.value("track"));
	Track* track = op.contains("channel") ? nullptr : findTrack(owner);
	EffectChain* chain = op.contains("channel") ? effectChainOf(owner) : track ? effectsOf(track) : nullptr;
	QDomDocument doc;
	if (!chain || !doc.setContent(op.value("xml").toString()) || doc.documentElement().tagName() != "fxchain") { return; }

	const QDomElement incoming = doc.documentElement();

	// Effect by effect instead of reloading the chain: effects that did not change keep their objects, so
	// their views, open windows and knobs stay as they are
	QDomDocument currentDoc;
	currentDoc.setContent(effectsXml(chain));
	const QList<QDomElement> currentElements = effectElements(currentDoc.documentElement());
	const std::vector<Effect*> old = chain->effects();
	auto stateText = [](QDomElement element) {
		element.removeAttribute("cid");
		return elementText(element);
	};

	gui::KeepWindowOrder keepWindowOrder;
	std::vector<Effect*> result;
	int created = 0, restored = 0;
	m_applyingRemote = true;
	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		for (const QDomElement& element : effectElements(incoming))
		{
			const QString id = element.attribute("cid");
			auto available = [&](std::size_t i) { return std::find(result.begin(), result.end(), old[i]) == result.end(); };
			int match = -1;
			for (std::size_t i = 0; i < old.size() && match < 0; ++i)
			{
				if (available(i) && !id.isEmpty() && old[i]->property(EffectIdProperty).toString() == id) { match = i; }
			}
			// Effects loaded from the project have no id until one side sends them: same plugin, in order
			for (std::size_t i = 0; i < old.size() && match < 0 && i < static_cast<std::size_t>(currentElements.size()); ++i)
			{
				if (available(i) && !old[i]->property("collabEffectIdShared").toBool()
					&& currentElements[i].attribute("name") == element.attribute("name")
					&& keyText(currentElements[i]) == keyText(element))
				{
					match = i;
				}
			}

			Effect* effect = nullptr;
			if (match >= 0)
			{
				effect = old[match];
				if (stateText(currentElements[match]) != stateText(element))
				{
					effect->restoreState(element);
					++restored;
				}
			}
			else
			{
				// As EffectChain::loadSettings() does, but a plugin missing here is kept as a placeholder
				// without LMMS's "Plugin not found" message box on every change
				EffectKey key(element.elementsByTagName("key").item(0).toElement());
				const bool available = !getPluginFactory()->pluginInfo(element.attribute("name").toUtf8()).isNull();
				if (!available) { log(QString{"effect %1 is not available here"}.arg(element.attribute("name"))); }
				effect = available ? Effect::instantiate(element.attribute("name"), chain, &key) : nullptr;
				if (effect && effect->isOkay() && effect->nodeName() == element.nodeName()) { effect->restoreState(element); }
				else
				{
					delete effect;
					effect = new DummyEffect(chain->parentModel(), element);
				}
				++created;
			}
			effect->setProperty(EffectIdProperty, id);
			effect->setProperty("collabEffectIdShared", true);
			// A Peak Controller effect keeps the id its controller is saved with (LMMS draws a new one)
			for (QDomElement c = element.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
			{
				if (c.hasAttribute("effectId")) { PeakController::setEffectId(effect, c.attribute("effectId").toInt()); }
			}
			result.push_back(effect);
		}

		for (Effect* effect : old)
		{
			if (std::find(result.begin(), result.end(), effect) == result.end()) { chain->removeEffect(effect); }
		}
		for (Effect* effect : result)
		{
			if (std::find(chain->effects().begin(), chain->effects().end(), effect) == chain->effects().end())
			{
				chain->appendEffect(effect);
			}
		}
		for (std::size_t target = 0; target < result.size(); ++target)
		{
			while (std::find(chain->effects().begin(), chain->effects().end(), result[target]) - chain->effects().begin()
				> static_cast<std::ptrdiff_t>(target))
			{
				chain->moveUp(result[target]);
			}
		}
		chain->enabledModel()->loadSettings(incoming, "enabled");
	}
	m_applyingRemote = false;

	// Effect racks showing this chain drop the views of removed effects, create the new ones and follow the
	// chain's order (EffectRackView::update() is a private slot)
	if (auto gui = gui::getGUI(); gui && gui->mainWindow())
	{
		for (gui::EffectRackView* rack : gui->mainWindow()->findChildren<gui::EffectRackView*>())
		{
			if (rack->model() == chain) { QMetaObject::invokeMethod(rack, "update"); }
		}
	}
	// Only now, so that no new effect can reuse the address of a removed one while views still point to it
	int removed = 0;
	for (Effect* effect : old)
	{
		if (std::find(result.begin(), result.end(), effect) == result.end())
		{
			effect->deleteLater();
			++removed;
		}
	}
	log(QString{"remote effects for %1: %2 effect(s), %3 new, %4 updated, %5 removed"}
		.arg(proto::idString(owner)).arg(chain->effects().size()).arg(created).arg(restored).arg(removed));
	resetParamBaselines(owner, "fx:");
	if (track) { rebasePlugins(track); }
	else { rebaseEffects(owner, chain); }
}


collab_id_t effectOwner(const Effect* effect)
{
	auto holds = [effect](const EffectChain* chain) {
		return chain && std::find(chain->effects().begin(), chain->effects().end(), effect) != chain->effects().end();
	};
	for (const TrackContainer* container : {static_cast<TrackContainer*>(Engine::getSong()),
		static_cast<TrackContainer*>(Engine::patternStore())})
	{
		for (Track* track : container->tracks())
		{
			if (holds(effectsOf(track))) { return track->collabId(); }
		}
	}
	for (int i = 0; i < static_cast<int>(Engine::mixer()->numChannels()); ++i)
	{
		MixerChannel* channel = Engine::mixer()->mixerChannel(i);
		if (holds(&channel->m_fxChain)) { return channel->collabId(); }
	}
	return 0;
}


EffectChain* effectChainOf(collab_id_t owner)
{
	if (auto track = const_cast<Track*>(static_cast<const Track*>(findOwner(IdScope::Track, owner)))) { return effectsOf(track); }
	MixerChannel* channel = findMixerChannel(owner);
	return channel ? &channel->m_fxChain : nullptr;
}


QString effectOwnerName(collab_id_t owner)
{
	if (auto track = static_cast<const Track*>(findOwner(IdScope::Track, owner))) { return track->name(); }
	MixerChannel* channel = findMixerChannel(owner);
	return channel ? channel->m_name : QString{};
}


int effectIndex(const Effect* effect)
{
	const EffectChain* chain = effectChainOf(effectOwner(effect));
	if (!chain) { return -1; }
	return static_cast<int>(std::find(chain->effects().begin(), chain->effects().end(), effect) - chain->effects().begin());
}


Effect* effectAt(collab_id_t owner, int index)
{
	const EffectChain* chain = effectChainOf(owner);
	if (!chain || index < 0 || index >= static_cast<int>(chain->effects().size())) { return nullptr; }
	return chain->effects()[index];
}

} // namespace lmms::collab
