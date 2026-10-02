/*
 * DummyInstrument.h - instrument used as fallback if an instrument couldn't
 *                     be loaded
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_DUMMY_INSTRUMENT_H
#define LMMS_DUMMY_INSTRUMENT_H

#include "Instrument.h"
#include "InstrumentView.h"
#include "Engine.h"

#include <cstring>

#include <QDomDocument>

#include "AudioEngine.h"


namespace lmms
{


class DummyInstrument : public Instrument
{
public:
	//! @p missingPlugin: the plugin this stands for, when it is not available here
	DummyInstrument( InstrumentTrack * _instrument_track, const QString& missingPlugin = {} ) :
		Instrument( _instrument_track, nullptr ),
		m_missingPlugin(missingPlugin)
	{
	}

	//! The plugin's own settings are kept as they were loaded and saved again unchanged, so a project that
	//! uses a plugin this computer does not have keeps them (for whoever has it, e.g. a collaborator)
	QDomElement saveState(QDomDocument& doc, QDomElement& parent) override
	{
		const QDomElement original = m_original.documentElement();
		if (original.isNull()) { return Instrument::saveState(doc, parent); }
		QDomElement copy = doc.importNode(original, true).toElement();
		parent.appendChild(copy);
		return copy;
	}

	void restoreState(const QDomElement& element) override
	{
		m_original = QDomDocument{};
		if (!element.isNull() && !m_missingPlugin.isEmpty())
		{
			m_original.appendChild(m_original.importNode(element, true));
		}
	}

	QString pluginName() const override
	{
		return m_missingPlugin.isEmpty() ? Instrument::pluginName() : m_missingPlugin;
	}

	const QString& missingPlugin() const { return m_missingPlugin; }

	~DummyInstrument() override = default;

	void playNote( NotePlayHandle*, SampleFrame* buffer ) override
	{
		zeroSampleFrames(buffer, Engine::audioEngine()->framesPerPeriod());
	}

	void saveSettings( QDomDocument &, QDomElement & ) override
	{
	}

	void loadSettings( const QDomElement & ) override
	{
	}

	QString nodeName() const override
	{
		return "dummyinstrument";
	}

	gui::PluginView * instantiateView( QWidget * _parent ) override
	{
		return new gui::InstrumentViewFixedSize( this, _parent );
	}

private:
	QString m_missingPlugin;
	QDomDocument m_original;
} ;


} // namespace lmms

#endif // LMMS_DUMMY_INSTRUMENT_H
