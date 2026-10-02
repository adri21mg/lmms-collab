/*
 * CollabConnection.h - opening a connection to a collaboration server: encryption, certificate, password
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

#ifndef LMMS_COLLAB_CONNECTION_H
#define LMMS_COLLAB_CONNECTION_H

#include <memory>

#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include "CollabProtocol.h"
#include "lmms_export.h"

class QSslSocket;

namespace lmms::collab
{

/**
 * Everything before the session itself, shared by the session and the project list:
 *  - encryption: TLS is tried first. A server without it closes that attempt at once, and the connection is
 *    made again without encryption; a server that had a certificate before is never used without one.
 *  - the server's certificate: remembered the first time (like SSH); a different one later is refused, since
 *    someone could be pretending to be the server.
 *  - hello, and the password when the server asks for it (only an answer derived from it travels).
 */
class LMMS_EXPORT CollabConnection : public QObject
{
	Q_OBJECT
public:
	enum class Failure
	{
		Network,             //!< cannot reach it, or the connection dropped (trying again later may help)
		Refused,             //!< the server said no (e.g. another protocol version)
		PasswordNeeded,
		WrongPassword,
		CertificateChanged,  //!< not the certificate remembered for this server
		NoLongerEncrypted    //!< this server was encrypted before and is not now
	};

	explicit CollabConnection(QObject* parent = nullptr);
	~CollabConnection() override;

	//! Connects and says @p hello; @p passwordKey (proto::passwordKey) answers the server's challenge if it asks
	void open(const QString& host, quint16 port, const QJsonObject& hello, const QByteArray& passwordKey);

	//! After ready(): the socket, for the caller to use from now on, and data received after the welcome
	std::unique_ptr<QSslSocket> takeSocket(QByteArray& rest);

	bool isEncrypted() const { return m_encrypted; }
	//! SHA-256 of the server's certificate, "ab:cd:..." (empty if not encrypted)
	QString fingerprint() const { return m_fingerprint; }

	//! The certificate remembered for a server ("" if none yet)
	static QString pinnedFingerprint(const QString& host, quint16 port);
	//! Remembers (or replaces, when the user trusts a new one) a server's certificate
	static void pin(const QString& host, quint16 port, const QString& fingerprint);

signals:
	void ready(const QJsonObject& welcome);
	void failed(lmms::collab::CollabConnection::Failure failure, const QString& message);

private:
	void start(bool encrypted);
	void onReadable();
	void onSocketError();
	void fail(Failure failure, const QString& message);

	std::unique_ptr<QSslSocket> m_socket;
	proto::FrameDecoder m_decoder;
	QTimer m_timeout;
	QString m_host;
	quint16 m_port = 0;
	QJsonObject m_hello;
	QByteArray m_passwordKey;
	bool m_tryingTls = false;
	bool m_tcpConnected = false;
	bool m_encrypted = false;
	bool m_done = false;
	QString m_fingerprint;
};

} // namespace lmms::collab

#endif // LMMS_COLLAB_CONNECTION_H
