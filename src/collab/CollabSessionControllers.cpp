/*
 * CollabSessionControllers.cpp - synchronization of controllers (milestone M5c)
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

// The LFO controllers of the Controller Rack are part of the synchronized structure (added, removed,
// renamed); their knobs are parameters like any other, owned by the controller. Peak Controllers come and
// go with their effect, which is synchronized with its effect chain; they are named after that effect.
// MIDI controllers belong to each user's own devices and are never shared: what they do to a knob is sent
// as if the user turned it by hand.
//
// Which controller a parameter follows is compared with the baseline each tick and sent as param.link.

#include <QDomDocument>

#include "CollabSession.h"
#include "CollabSessionUtil.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "ControllerRackView.h"
#include "ControllerView.h"
#include "Effect.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "PeakController.h"
#include "Song.h"

namespace lmms::collab
{

namespace
{

//! Controllers that are part of the structure: made in the Controller Rack
bool isRackController(const Controller* controller)
{
	return controller->type() == Controller::ControllerType::Lfo;
}

Controller* findRackController(collab_id_t id)
{
	for (Controller* controller : Engine::getSong()->controllers())
	{
		if (isRackController(controller) && controller->collabId() == id) { return controller; }
	}
	return nullptr;
}

bool followsMidi(const AutomatableModel* model)
{
	ControllerConnection* connection = model->controllerConnection();
	return connection && connection->getController()
		&& connection->getController()->type() == Controller::ControllerType::Midi;
}

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


QString controllerName(Controller* controller)
{
	if (!controller) { return {}; }
	if (auto peak = dynamic_cast<PeakController*>(controller))
	{
		const Effect* effect = peak->effect();
		const collab_id_t owner = effect ? effectOwner(effect) : 0;
		return owner ? QString{"p:%1:%2"}.arg(proto::idString(owner)).arg(effectIndex(effect)) : QString{};
	}
	return isRackController(controller) ? proto::idString(controller->collabId()) : QString{};
}


Controller* findController(const QString& name)
{
	if (name.startsWith("p:"))
	{
		const Effect* effect = effectAt(idFromString(name.section(':', 1, 1)), name.section(':', 2).toInt());
		for (Controller* controller : Engine::getSong()->controllers())
		{
			auto peak = dynamic_cast<PeakController*>(controller);
			if (effect && peak && peak->effect() == effect) { return controller; }
		}
		return nullptr;
	}
	return findRackController(idFromString(name));
}


void CollabSession::addControllerStructure(Structure& s)
{
	for (Controller* controller : Engine::getSong()->controllers())
	{
		if (const QString name = controllerName(controller); !name.isEmpty())
		{
			s.allControllers.append(name);
			if (name.startsWith("p:")) { s.peakNames.insert(name, controller->name()); }
		}
		if (!isRackController(controller)) { continue; }
		s.controllers.append(controller->collabId());
		s.controllerFields.insert(controller->collabId(), QJsonObject{{"name", controller->name()}});
	}
}


QString CollabSession::controllersXml()
{
	QDomDocument doc;
	QDomElement controllers = doc.createElement("controllers");
	doc.appendChild(controllers);
	for (Controller* controller : Engine::getSong()->controllers()) { controller->saveState(doc, controllers); }
	return elementToString(controllers);
}


void CollabSession::flushControllers(const Structure& current, QJsonArray& ops, qint64 ctx)
{
	const Structure& old = m_structure;
	const int opsBefore = ops.size();
	for (const collab_id_t id : old.controllers)
	{
		if (!current.controllerFields.contains(id))
		{
			ops.append(QJsonObject{{"op", proto::op::ControllerRemove}, {"id", proto::idString(id)}});
		}
	}
	for (const collab_id_t id : current.controllers)
	{
		Controller* controller = findRackController(id);
		if (!controller) { continue; }
		if (!old.controllerFields.contains(id))
		{
			QDomDocument doc;
			QDomElement parent = doc.createElement("collab");
			doc.appendChild(parent);
			controller->saveState(doc, parent);
			ops.append(QJsonObject{{"op", proto::op::ControllerAdd}, {"id", proto::idString(id)},
				{"xml", elementToString(parent.firstChildElement())}});
			continue;
		}
		const QJsonObject changed = changedFields(old.controllerFields.value(id), current.controllerFields.value(id));
		if (changed.isEmpty()) { continue; }
		ops.append(QJsonObject{{"op", proto::op::ControllerSet}, {"id", proto::idString(id)}, {"v", changed}});
		for (auto it = changed.begin(); it != changed.end(); ++it) { m_pendingStructure[pendingKey('k', id, it.key())] = ctx; }
	}
	// Peak Controllers come and go with their effect, but can be renamed
	for (auto it = current.peakNames.cbegin(); it != current.peakNames.cend(); ++it)
	{
		if (!old.peakNames.contains(it.key()) || old.peakNames.value(it.key()) == it.value()) { continue; }
		ops.append(QJsonObject{{"op", proto::op::ControllerSet}, {"ref", it.key()}, {"v", QJsonObject{{"name", it.value()}}}});
		m_pendingStructure[pendingKey('k', 0, it.key())] = ctx;
	}
	// Also when only Peak Controllers came or went (with their effect): connections refer to positions in it
	if (ops.size() != opsBefore || current.allControllers != old.allControllers)
	{
		ops.append(QJsonObject{{"op", proto::op::ControllersState}, {"xml", controllersXml()}});
		m_paramIndexAge.invalidate();
	}
}


void CollabSession::applyRemoteControllerOp(const QJsonObject& op)
{
	const QString type = op.value("op").toString();
	const collab_id_t id = proto::parseId(op.value("id"));
	if (type == proto::op::ControllerAdd)
	{
		QDomDocument doc;
		if (id == 0 || findRackController(id) || !doc.setContent(op.value("xml").toString())) { return; }
		Controller* controller = Controller::create(doc.documentElement(), Engine::getSong());
		if (!controller || !isRackController(controller)) { return; }
		controller->setCollabId(id);
		Engine::getSong()->addController(controller); // the Controller Rack adds its view
	}
	else if (type == proto::op::ControllerRemove)
	{
		if (Controller* controller = findRackController(id)) { Engine::getSong()->removeController(controller); }
	}
	else if (type == proto::op::ControllerSet)
	{
		// An LFO by id, or a Peak Controller by the name of its effect ("ref")
		const QString ref = op.value("ref").toString();
		Controller* controller = ref.isEmpty() ? findRackController(id) : findController(ref);
		const QJsonObject fields = op.value("v").toObject();
		const QString pending = ref.isEmpty() ? pendingKey('k', id, "name") : pendingKey('k', 0, ref);
		if (!controller || !fields.contains("name") || m_pendingStructure.contains(pending)) { return; }
		controller->setName(fields.value("name").toString());
		if (auto gui = gui::getGUI(); gui && gui->getControllerRackView())
		{
			for (gui::ControllerView* view : gui->getControllerRackView()->findChildren<gui::ControllerView*>())
			{
				if (view->getController() == controller) { view->updateName(); }
			}
		}
	}
}


std::vector<std::pair<QString, AutomatableModel*>> CollabSession::enumerateControllerParams(Controller* controller)
{
	std::vector<std::pair<QString, AutomatableModel*>> params;
	int n = 0;
	visitModels(controller, "k", n, params);
	return params;
}


QString CollabSession::sharedConnection(const AutomatableModel* model)
{
	ControllerConnection* connection = model->controllerConnection();
	if (!connection || followsMidi(model)) { return {}; }
	return controllerName(connection->getController());
}


bool CollabSession::applyLink(AutomatableModel* model, const QString& name)
{
	if (name.isEmpty())
	{
		// Disconnect, unless it follows this user's own MIDI device
		if (model->controllerConnection() && !followsMidi(model))
		{
			delete model->controllerConnection();
			model->setControllerConnection(nullptr);
		}
		return true;
	}
	Controller* controller = findController(name);
	if (!controller) { return false; }
	if (model->controllerConnection() && !followsMidi(model)) { model->controllerConnection()->setController(controller); }
	else
	{
		// Like LMMS' "Connect to controller" dialog (a MIDI connection here gives way to the shared one)
		delete model->controllerConnection();
		model->setControllerConnection(new ControllerConnection(controller));
	}
	return true;
}


void CollabSession::applyRemoteLink(const QJsonObject& op, qint64 seq)
{
	const QString owner = op.value("owner").toString();
	const ParamKey key{owner == proto::SongOwner ? 0 : proto::parseId(op.value("owner")), op.value("path").toString()};
	AutomatableModel* model = findParam(key);
	if (!model) { return; }
	if (!m_pendingLinks.contains(key)) // our unacknowledged change wins
	{
		const QString controller = op.value("controller").toString();
		m_applyingRemote = true;
		const bool applied = applyLink(model, controller);
		m_applyingRemote = false;
		if (applied) { m_unresolvedLinks.remove(key); }
		else { m_unresolvedLinks.insert(key, controller); }
		m_linkBaseline[key] = sharedConnection(model);
		log(QString{"remote link %1 %2 -> %3%4"}.arg(owner, key.second, controller.isEmpty() ? "none" : controller,
			applied ? "" : " (not found yet)"));
	}
	// As for parameter values: whoever made the latest change stores the owner's settings
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


void CollabSession::retryLinks()
{
	for (auto it = m_unresolvedLinks.begin(); it != m_unresolvedLinks.end();)
	{
		AutomatableModel* model = findParam(it.key());
		if (model && applyLink(model, it.value()))
		{
			m_linkBaseline[it.key()] = sharedConnection(model);
			it = m_unresolvedLinks.erase(it);
		}
		else { ++it; }
	}
}

} // namespace lmms::collab
