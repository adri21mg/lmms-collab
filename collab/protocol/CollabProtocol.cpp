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

#include <QJsonDocument>
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
