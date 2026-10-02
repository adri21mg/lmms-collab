/*
 * CollabConnection.cpp - opening a connection to a collaboration server: encryption, certificate, password
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

#include "CollabConnection.h"

#include <QCryptographicHash>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStringList>

#include "ConfigManager.h"

namespace lmms::collab
{

namespace
{
constexpr int TimeoutMs = 10000;
constexpr auto ConfigSection = "collab";
constexpr auto CertificatesKey = "certificates"; // "host:port=fingerprint;..."

QString serverKey(const QString& host, quint16 port)
{
	return host.trimmed().toLower() + ":" + QString::number(port);
}
}


CollabConnection::CollabConnection(QObject* parent) :
	QObject(parent)
{
	m_timeout.setSingleShot(true);
	connect(&m_timeout, &QTimer::timeout, this, [this] { fail(Failure::Network, tr("The server does not answer.")); });
}


CollabConnection::~CollabConnection()
{
	if (m_socket)
	{
		m_socket->disconnect(this);
		m_socket->abort();
	}
}


void CollabConnection::open(const QString& host, quint16 port, const QJsonObject& hello, const QByteArray& passwordKey)
{
	m_host = host;
	m_port = port;
	m_hello = hello;
	m_passwordKey = passwordKey;
	m_timeout.start(TimeoutMs);
	start(QSslSocket::supportsSsl());
}


void CollabConnection::start(bool encrypted)
{
	if (m_socket)
	{
		m_socket->disconnect(this);
		m_socket->abort();
		m_socket.release()->deleteLater(); // this may run inside one of its own signals
	}
	m_tryingTls = encrypted;
	m_tcpConnected = false;
	m_encrypted = false;
	m_fingerprint.clear();
	m_decoder = proto::FrameDecoder{};
	m_socket = std::make_unique<QSslSocket>();

	auto sendHello = [this] { m_socket->write(proto::encodeJsonFrame(m_hello)); };
	connect(m_socket.get(), &QSslSocket::connected, this, [this, sendHello] {
		m_tcpConnected = true;
		if (!m_tryingTls) { sendHello(); }
	});
	connect(m_socket.get(), &QSslSocket::encrypted, this, [this, sendHello] {
		m_encrypted = true;
		m_fingerprint = QString::fromLatin1(
			m_socket->peerCertificate().digest(QCryptographicHash::Sha256).toHex(':'));
		const QString pinned = pinnedFingerprint(m_host, m_port);
		if (pinned.isEmpty()) { pin(m_host, m_port, m_fingerprint); } // the first time: remembered
		else if (pinned != m_fingerprint)
		{
			return fail(Failure::CertificateChanged, tr("The server's certificate is not the one it had before. "
				"Someone could be pretending to be the server; if its owner made a new certificate, you can trust it."));
		}
		sendHello();
	});
	connect(m_socket.get(), &QSslSocket::readyRead, this, &CollabConnection::onReadable);
	connect(m_socket.get(), &QSslSocket::errorOccurred, this, &CollabConnection::onSocketError);
	connect(m_socket.get(), &QSslSocket::disconnected, this, &CollabConnection::onSocketError);

	if (encrypted)
	{
		QSslConfiguration configuration = m_socket->sslConfiguration();
		configuration.setProtocol(QSsl::TlsV1_2OrLater);
		// Collaboration servers have their own certificate, not one signed by an authority: it is checked
		// against the one remembered for this server instead (see encrypted above)
		configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
		m_socket->setSslConfiguration(configuration);
		m_socket->connectToHostEncrypted(m_host, m_port);
	}
	else { m_socket->connectToHost(m_host, m_port); }
}


void CollabConnection::onSocketError()
{
	if (m_done || !m_socket) { return; }
	if (m_tryingTls && m_tcpConnected && !m_encrypted)
	{
		// The server is there but closed the encrypted attempt: it has no encryption
		if (!pinnedFingerprint(m_host, m_port).isEmpty())
		{
			return fail(Failure::NoLongerEncrypted, tr("This server used to be encrypted and is not now. "
				"Someone could be pretending to be the server, so this LMMS does not connect to it."));
		}
		return start(false);
	}
	fail(Failure::Network, m_socket->errorString());
}


void CollabConnection::onReadable()
{
	if (m_done || !m_socket) { return; }
	m_decoder.append(m_socket->readAll());
	proto::FrameType type;
	QByteArray payload;
	while (!m_done && m_decoder.next(type, payload))
	{
		const auto message = proto::parseMessage(payload);
		if (type != proto::FrameType::Json || !message)
		{
			return fail(Failure::Refused, tr("The server sent an invalid message."));
		}
		const QString t = message->value("t").toString();
		if (t == proto::msg::Auth)
		{
			if (m_passwordKey.isEmpty())
			{
				return fail(Failure::PasswordNeeded, tr("This server needs a password."));
			}
			const QByteArray challenge = QByteArray::fromHex(message->value("challenge").toString().toLatin1());
			m_socket->write(proto::encodeJsonFrame({{"t", proto::msg::Auth},
				{"response", QString::fromLatin1(proto::authResponse(m_passwordKey, challenge).toHex())}}));
		}
		else if (t == proto::msg::Welcome)
		{
			m_done = true;
			m_timeout.stop();
			emit ready(*message);
			return;
		}
		else if (t == proto::msg::Error)
		{
			const QString text = message->value("message").toString();
			return fail(text == "wrong password" ? Failure::WrongPassword : Failure::Refused,
				text == "wrong password" ? tr("Wrong password.") : text);
		}
	}
	if (m_decoder.hasError()) { fail(Failure::Refused, tr("The server sent an invalid message.")); }
}


void CollabConnection::fail(Failure failure, const QString& message)
{
	if (m_done) { return; }
	m_done = true;
	m_timeout.stop();
	if (m_socket)
	{
		m_socket->disconnect(this);
		m_socket->abort();
	}
	emit failed(failure, message);
}


std::unique_ptr<QSslSocket> CollabConnection::takeSocket(QByteArray& rest)
{
	if (m_socket) { m_socket->disconnect(this); }
	rest = m_decoder.takeBuffer();
	return std::move(m_socket);
}


QString CollabConnection::pinnedFingerprint(const QString& host, quint16 port)
{
	const QString key = serverKey(host, port);
	for (const QString& entry : ConfigManager::inst()->value(ConfigSection, CertificatesKey).split(';', Qt::SkipEmptyParts))
	{
		if (entry.section('=', 0, 0) == key) { return entry.section('=', 1); }
	}
	return {};
}


void CollabConnection::pin(const QString& host, quint16 port, const QString& fingerprint)
{
	const QString key = serverKey(host, port);
	QStringList entries;
	for (const QString& entry : ConfigManager::inst()->value(ConfigSection, CertificatesKey).split(';', Qt::SkipEmptyParts))
	{
		if (entry.section('=', 0, 0) != key) { entries.append(entry); }
	}
	entries.append(key + "=" + fingerprint);
	ConfigManager::inst()->setValue(ConfigSection, CertificatesKey, entries.join(';'));
}

} // namespace lmms::collab
