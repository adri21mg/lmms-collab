/*
 * CollabSessionAutomation.cpp - synchronization of automation clips (milestone M5b)
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

// Automation tracks and clips are part of the synchronized structure like any other track and clip. What
// an automation clip holds (its nodes, curve and the parameters it automates) is compared as a whole each
// tick and sent with automation.set.
//
// LMMS names automated parameters by journal id, which differs on every client. They travel as the
// parameter's owner and path instead (the names param.set uses), both in automation.set and in the XML of
// tracks and clips (<object owner="..." path="..."/>).

#include <QDomDocument>
#include <QJsonDocument>

#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationEditor.h"
#include "CollabSession.h"
#include "CollabSessionUtil.h"
#include "Engine.h"
#include "GuiApplication.h"

namespace lmms::collab
{

namespace
{

QString number(double value)
{
	return QString::number(value, 'g', 9);
}

} // namespace


QJsonObject CollabSession::automationContent(const AutomationClip* clip) const
{
	QJsonArray nodes;
	const auto& timeMap = clip->getTimeMap();
	for (auto it = timeMap.cbegin(); it != timeMap.cend(); ++it)
	{
		const AutomationNode& node = it.value();
		nodes.append(QJsonArray{it.key(), static_cast<double>(node.getInValue()),
			static_cast<double>(node.getOutValue()), static_cast<double>(node.getInTangent()),
			static_cast<double>(node.getOutTangent()), node.lockedTangents() ? 1 : 0});
	}
	QJsonArray objects;
	for (const auto& model : clip->objects())
	{
		if (!model) { continue; }
		if (const auto key = paramKeyOf(model))
		{
			objects.append(QJsonObject{{"owner", key->first == 0 ? QString{proto::SongOwner} : proto::idString(key->first)},
				{"path", key->second}});
		}
	}
	// Parameters it will automate once they exist here: still part of what it holds
	for (const QJsonValue& missing : m_unresolvedAutomation.value(clip->collabId())) { objects.append(missing); }
	return {{"prog", static_cast<int>(clip->progressionType())}, {"tens", static_cast<double>(clip->getTension())},
		{"nodes", nodes}, {"objects", objects}};
}


QString CollabSession::automationSignature(const AutomationClip* clip) const
{
	return QString::fromUtf8(QJsonDocument{automationContent(clip)}.toJson(QJsonDocument::Compact));
}


void CollabSession::applyAutomationContent(AutomationClip* clip, const QJsonObject& content)
{
	// Through LMMS' own loading, which takes the clip's lock against the audio thread; everything but the
	// content stays as it is
	QDomDocument doc;
	QDomElement e = doc.createElement("automationclip");
	e.setAttribute("pos", clip->startPosition().getTicks());
	e.setAttribute("len", clip->length().getTicks());
	e.setAttribute("off", clip->startTimeOffset().getTicks());
	e.setAttribute("name", clip->name());
	e.setAttribute("mute", clip->isMuted() ? 1 : 0);
	e.setAttribute("autoresize", clip->getAutoResize() ? 1 : 0);
	if (clip->color()) { e.setAttribute("color", clip->color()->name()); }
	e.setAttribute("prog", content.value("prog").toInt());
	e.setAttribute("tens", number(content.value("tens").toDouble()));
	for (const QJsonValue& value : content.value("nodes").toArray())
	{
		const QJsonArray node = value.toArray();
		QDomElement time = doc.createElement("time");
		time.setAttribute("pos", node[0].toInt());
		time.setAttribute("value", number(node[1].toDouble()));
		time.setAttribute("outValue", number(node[2].toDouble()));
		time.setAttribute("inTan", number(node[3].toDouble()));
		time.setAttribute("outTan", number(node[4].toDouble()));
		time.setAttribute("lockedTan", node[5].toInt());
		e.appendChild(time);
	}
	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		clip->loadSettings(e);
	}
	setAutomationObjects(clip, content.value("objects").toArray());
	// The Automation Editor only reads the tension and curve type when a clip is opened
	if (auto gui = gui::getGUI(); gui && gui->automationEditor() && gui->automationEditor()->currentClip() == clip)
	{
		gui->automationEditor()->updateClipSettings();
	}
}


void CollabSession::setAutomationObjects(AutomationClip* clip, const QJsonArray& objects)
{
	QJsonArray missing;
	std::vector<AutomatableModel*> models;
	for (const QJsonValue& value : objects)
	{
		const QJsonObject object = value.toObject();
		const QString owner = object.value("owner").toString();
		const ParamKey key{owner == proto::SongOwner ? 0 : proto::parseId(object.value("owner")), object.value("path").toString()};
		if (AutomatableModel* model = findParam(key)) { models.push_back(model); }
		else { missing.append(object); }
	}
	clip->clearObjects();
	for (AutomatableModel* model : models) { clip->addObject(model, true); }
	if (missing.isEmpty()) { m_unresolvedAutomation.remove(clip->collabId()); }
	else
	{
		m_unresolvedAutomation.insert(clip->collabId(), missing);
		log(QString{"automation %1: %2 parameter(s) not found yet"}.arg(proto::idString(clip->collabId())).arg(missing.size()));
	}
	emit clip->dataChanged();
}


void CollabSession::retryAutomationObjects()
{
	for (auto it = m_unresolvedAutomation.begin(); it != m_unresolvedAutomation.end();)
	{
		auto clip = dynamic_cast<AutomationClip*>(findClip(it.key()));
		if (!clip)
		{
			it = m_unresolvedAutomation.erase(it);
			continue;
		}
		QJsonArray still;
		for (const QJsonValue& value : it.value())
		{
			const QJsonObject object = value.toObject();
			const QString owner = object.value("owner").toString();
			const ParamKey key{owner == proto::SongOwner ? 0 : proto::parseId(object.value("owner")), object.value("path").toString()};
			if (AutomatableModel* model = findParam(key)) { clip->addObject(model, true); }
			else { still.append(object); }
		}
		if (still.size() != it.value().size()) { emit clip->dataChanged(); }
		if (still.isEmpty()) { it = m_unresolvedAutomation.erase(it); }
		else
		{
			it.value() = still;
			++it;
		}
	}
}


void CollabSession::annotateAutomation(QDomElement root)
{
	CollabSession* session = instance();
	std::vector<QDomElement> clips;
	if (root.tagName() == "automationclip") { clips.push_back(root); }
	const QDomNodeList list = root.elementsByTagName("automationclip");
	for (int i = 0; i < list.size(); ++i) { clips.push_back(list.at(i).toElement()); }

	for (QDomElement e : clips)
	{
		auto clip = dynamic_cast<const AutomationClip*>(findClip(idFromString(e.attribute(IdAttribute))));
		if (!clip) { continue; }
		for (QDomElement o = e.firstChildElement("object"); !o.isNull(); o = e.firstChildElement("object")) { e.removeChild(o); }
		for (const QJsonValue& value : session->automationContent(clip).value("objects").toArray())
		{
			QDomElement object = e.ownerDocument().createElement("object");
			object.setAttribute("owner", value.toObject().value("owner").toString());
			object.setAttribute("path", value.toObject().value("path").toString());
			e.appendChild(object);
		}
	}
}


void CollabSession::resolveAutomation(const QDomElement& root)
{
	std::vector<QDomElement> clips;
	if (root.tagName() == "automationclip") { clips.push_back(root); }
	const QDomNodeList list = root.elementsByTagName("automationclip");
	for (int i = 0; i < list.size(); ++i) { clips.push_back(list.at(i).toElement()); }

	for (const QDomElement& e : clips)
	{
		auto clip = dynamic_cast<AutomationClip*>(findClip(idFromString(e.attribute(IdAttribute))));
		if (!clip) { continue; }
		QJsonArray objects;
		bool journalIds = false;
		for (QDomElement o = e.firstChildElement("object"); !o.isNull(); o = o.nextSiblingElement("object"))
		{
			if (!o.hasAttribute("owner"))
			{
				journalIds = true;
				continue;
			}
			objects.append(QJsonObject{{"owner", o.attribute("owner")}, {"path", o.attribute("path")}});
		}
		// Only journal ids: a project saved before collaboration, whose ids LMMS resolved while loading it
		if (journalIds && objects.isEmpty()) { continue; }
		setAutomationObjects(clip, objects);
	}
}

} // namespace lmms::collab
