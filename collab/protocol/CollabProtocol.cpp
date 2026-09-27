/*
 * CollabProtocol.cpp - wire protocol shared by the LMMS collaboration client and server
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

#include "CollabProtocol.h"

#include <algorithm>
#include <tuple>

#include <QJsonDocument>
#include <QRegularExpression>
#include <QtEndian>

namespace lmms::collab::proto
{

namespace
{
constexpr int HeaderSize = 5; // uint32 length + uint8 type
}


QByteArray encodeJsonFrame(const QJsonObject& message)
{
	const QByteArray payload = QJsonDocument{message}.toJson(QJsonDocument::Compact);
	QByteArray frame(HeaderSize, Qt::Uninitialized);
	qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
	frame[4] = static_cast<char>(FrameType::Json);
	frame.append(payload);
	return frame;
}


void FrameDecoder::append(const QByteArray& data)
{
	m_buffer.append(data);
}


bool FrameDecoder::next(FrameType& type, QByteArray& payload)
{
	if (m_error || m_buffer.size() < HeaderSize) { return false; }

	const auto length = qFromBigEndian<quint32>(m_buffer.constData());
	const auto rawType = static_cast<quint8>(m_buffer[4]);
	if (length > MaxFrameSize || rawType > static_cast<quint8>(FrameType::Binary))
	{
		m_error = true;
		m_buffer.clear();
		return false;
	}
	if (static_cast<quint64>(m_buffer.size()) < HeaderSize + static_cast<quint64>(length)) { return false; }

	type = static_cast<FrameType>(rawType);
	payload = m_buffer.mid(HeaderSize, static_cast<qsizetype>(length));
	m_buffer.remove(0, HeaderSize + static_cast<qsizetype>(length));
	return true;
}


std::optional<QJsonObject> parseMessage(const QByteArray& payload)
{
	QJsonParseError error{};
	const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) { return std::nullopt; }
	QJsonObject obj = doc.object();
	if (!obj.value("t").isString()) { return std::nullopt; }
	return obj;
}


bool NoteValues::isComplete() const
{
	for (const auto& f : fields)
	{
		if (!f) { return false; }
	}
	return true;
}


bool NoteValues::isEmpty() const
{
	for (const auto& f : fields)
	{
		if (f) { return false; }
	}
	return true;
}


QJsonObject NoteValues::toJson() const
{
	QJsonObject obj;
	for (int i = 0; i < FieldCount; ++i)
	{
		if (fields[i]) { obj.insert(FieldNames[i], *fields[i]); }
	}
	return obj;
}


std::optional<NoteValues> NoteValues::fromJson(const QJsonObject& obj)
{
	NoteValues values;
	for (int i = 0; i < FieldCount; ++i)
	{
		const QJsonValue v = obj.value(FieldNames[i]);
		if (v.isUndefined()) { continue; }
		if (!v.isDouble()) { return std::nullopt; }
		const double d = v.toDouble();
		const int n = static_cast<int>(d);
		if (static_cast<double>(n) != d || !isValid(static_cast<Field>(i), n)) { return std::nullopt; }
		values.fields[i] = n;
	}
	return values;
}


bool NoteValues::isValid(Field f, int value)
{
	constexpr int MaxTicks = 1 << 30;
	switch (f)
	{
	case Key: return value >= 0 && value <= 128;
	case Pos: return value >= 0 && value < MaxTicks;
	case Len: return value > -MaxTicks && value < MaxTicks;
	case Vol: return value >= 0 && value <= 200;
	case Pan: return value >= -100 && value <= 100;
	case Type: return value == 0 || value == 1;
	default: return false;
	}
}


const std::vector<FieldSpec>& trackFields()
{
	using K = FieldSpec::Kind;
	static const std::vector<FieldSpec> fields = {
		{"name", K::String}, {"muted", K::Bool}, {"color", K::Color}};
	return fields;
}


const std::vector<FieldSpec>& clipFields()
{
	using K = FieldSpec::Kind;
	constexpr int MaxTicks = 1 << 30;
	static const std::vector<FieldSpec> fields = {
		{"pos", K::Int, 0, MaxTicks}, {"len", K::Int, 0, MaxTicks}, {"off", K::Int, -MaxTicks, MaxTicks},
		{"name", K::String}, {"color", K::Color}, {"muted", K::Bool}, {"autoresize", K::Bool},
		{"steps", K::Int, 1, 4096}};
	return fields;
}


bool validFields(const QJsonObject& values, const std::vector<FieldSpec>& spec)
{
	for (auto it = values.begin(); it != values.end(); ++it)
	{
		const auto f = std::find_if(spec.begin(), spec.end(), [&](const FieldSpec& s) { return it.key() == s.name; });
		if (f == spec.end()) { return false; }
		const QJsonValue v = it.value();
		switch (f->kind)
		{
		case FieldSpec::Kind::Int:
		{
			if (!v.isDouble()) { return false; }
			const double d = v.toDouble();
			if (d != static_cast<double>(static_cast<int>(d)) || d < f->min || d > f->max) { return false; }
			break;
		}
		case FieldSpec::Kind::Bool:
			if (!v.isBool()) { return false; }
			break;
		case FieldSpec::Kind::String:
			if (!v.isString() || v.toString().size() > 256) { return false; }
			break;
		case FieldSpec::Kind::Color:
		{
			static const QRegularExpression re{"^(#[0-9a-fA-F]{6})?$"};
			if (!v.isString() || !re.match(v.toString()).hasMatch()) { return false; }
			break;
		}
		}
	}
	return true;
}


const std::vector<SongParam>& songParams()
{
	static const std::vector<SongParam> params = {{"bpm", "bpm", 10, 999},
		{"num", "timesig_numerator", 1, 32}, {"den", "timesig_denominator", 1, 32},
		{"vol", "mastervol", 0, 200}, {"pitch", "masterpitch", -60, 60}};
	return params;
}


bool isValidParamPath(const QString& path)
{
	static const QRegularExpression re{"^(t|i|mt|fx):[0-9]{1,5}$"};
	return re.match(path).hasMatch();
}


std::optional<QString> validColor(const QJsonValue& value)
{
	static const QRegularExpression re{"^#[0-9a-fA-F]{6}$"};
	if (!value.isString() || !re.match(value.toString()).hasMatch()) { return std::nullopt; }
	return value.toString().toLower();
}


std::optional<QJsonObject> sanitizePresence(const QJsonObject& message)
{
	static const QRegularExpression windowRe{"^(song|mixer|notes|controllers|(pattern|pianoroll|instrument):[0-9a-f]{16})$"};
	static const QRegularExpression anchorRe{"^((track|head):[0-9a-f]{16}|(chan|fx):[0-9]{1,5})$"};
	static const QRegularExpression idRe{"^[0-9a-f]{16}$"};
	constexpr double MaxTicks = 1 << 30;
	constexpr double MaxPixels = 100000;

	auto number = [](const QJsonValue& v, double min, double max) -> std::optional<double> {
		if (!v.isDouble()) { return std::nullopt; }
		const double d = v.toDouble();
		return d >= min && d <= max ? std::optional{d} : std::nullopt;
	};

	QJsonObject result;
	const QJsonValue cursor = message.value("cursor");
	if (cursor.isObject())
	{
		const QJsonObject c = cursor.toObject();
		if (!windowRe.match(c.value("w").toString()).hasMatch()) { return std::nullopt; }
		QJsonObject clean{{"w", c.value("w")}};
		if (c.contains("a"))
		{
			if (!anchorRe.match(c.value("a").toString()).hasMatch()) { return std::nullopt; }
			clean.insert("a", c.value("a"));
		}
		const std::array<std::tuple<const char*, double, double>, 5> fields{{
			{"tick", -MaxTicks, MaxTicks}, {"key", 0, 200}, {"x", -MaxPixels, MaxPixels}, {"y", -MaxPixels, MaxPixels},
			{"tab", 0, 64}}};
		for (const auto& [name, min, max] : fields)
		{
			if (!c.contains(name)) { continue; }
			const auto v = number(c.value(name), min, max);
			if (!v) { return std::nullopt; }
			clean.insert(name, *v);
		}
		result.insert("cursor", clean);
	}
	else if (!cursor.isNull() && !cursor.isUndefined()) { return std::nullopt; }
	else { result.insert("cursor", QJsonValue::Null); }

	const QJsonValue play = message.value("play");
	if (play.isObject())
	{
		const QJsonObject p = play.toObject();
		QJsonObject clean{{"playing", p.value("playing").toBool()}};
		for (const char* name : {"song", "pattern", "tick"})
		{
			if (!p.contains(name)) { continue; }
			const auto tick = number(p.value(name), 0, MaxTicks);
			if (!tick) { return std::nullopt; }
			clean.insert(name, *tick);
		}
		if (p.contains("mode"))
		{
			const QString mode = p.value("mode").toString();
			if (mode != "song" && mode != "pattern" && mode != "clip") { return std::nullopt; }
			clean.insert("mode", mode);
		}
		for (const char* name : {"ref", "pref"})
		{
			if (!p.contains(name)) { continue; }
			if (!idRe.match(p.value(name).toString()).hasMatch()) { return std::nullopt; }
			clean.insert(name, p.value(name));
		}
		result.insert("play", clean);
	}
	else if (!play.isNull() && !play.isUndefined()) { return std::nullopt; }
	else { result.insert("play", QJsonValue::Null); }

	const QJsonValue view = message.value("view");
	if (view.isObject())
	{
		QJsonObject clean;
		if (view.toObject().contains("mixsel"))
		{
			const auto channel = number(view.toObject().value("mixsel"), 0, 10000);
			if (!channel) { return std::nullopt; }
			clean.insert("mixsel", *channel);
		}
		result.insert("view", clean);
	}
	else if (!view.isNull() && !view.isUndefined()) { return std::nullopt; }
	else { result.insert("view", QJsonValue::Null); }

	return result;
}


std::uint64_t parseId(const QJsonValue& value)
{
	if (!value.isString()) { return 0; }
	const QString s = value.toString();
	if (s.size() != 16) { return 0; }
	bool ok = false;
	const std::uint64_t id = s.toULongLong(&ok, 16);
	return ok ? id : 0;
}


QString idString(std::uint64_t id)
{
	return QString{"%1"}.arg(id, 16, 16, QChar{'0'});
}

} // namespace lmms::collab::proto
