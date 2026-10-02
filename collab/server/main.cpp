/*
 * main.cpp - entry point of the LMMS collaboration server
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

#include <atomic>
#include <csignal>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSysInfo>
#include <QTimer>

#include "CollabProtocol.h"
#include "CollabServer.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

std::atomic<bool> s_quitRequested{false};
std::atomic<bool> s_shutdownComplete{false};

#ifndef _WIN32
void requestQuit(int)
{
	s_quitRequested = true; // only async-signal-safe work here; the event loop polls the flag
}
#else
//! Ctrl+C, Ctrl+Break and closing the console window. For a closing window Windows terminates the
//! process as soon as this handler returns, so wait (bounded) until the projects have been saved.
BOOL WINAPI consoleHandler(DWORD event)
{
	s_quitRequested = true;
	if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT)
	{
		for (int i = 0; i < 40 && !s_shutdownComplete; ++i) { Sleep(100); }
	}
	return TRUE;
}
#endif

} // namespace


int main(int argc, char* argv[])
{
	QCoreApplication app{argc, argv};
	QCoreApplication::setApplicationName("lmms-collab-server");
	qSetMessagePattern("%{time yyyy-MM-dd hh:mm:ss} %{if-warning}WARNING %{endif}%{if-critical}ERROR %{endif}%{message}");

	QCommandLineParser parser;
	parser.setApplicationDescription("LMMS collaboration server (private sessions)");
	parser.addHelpOption();
	const QCommandLineOption portOption{"port", "TCP port (default 42871).", "port",
		QString::number(lmms::collab::proto::DefaultPort)};
	const QCommandLineOption listenOption{"listen",
		"Address to listen on. The default 127.0.0.1 only accepts connections from this computer.", "address",
		"127.0.0.1"};
	const QCommandLineOption dataOption{"data", "Directory where shared projects are stored.", "dir",
		"collab-data"};
	// Versions are always kept by the server; with Perforce they are also submitted there
	const QCommandLineOption p4PortOption{"p4port", "Also submit versions to this Perforce server (e.g. 127.0.0.1:1666).",
		"address"};
	const QCommandLineOption p4UserOption{"p4user", "Perforce user of the server (logged in once, with a ticket that "
		"does not expire).", "user"};
	const QCommandLineOption p4DepotOption{"p4depot", "Depot folder for the projects (e.g. //depot/music).", "path"};
	const QCommandLineOption p4ClientOption{"p4client", "Perforce workspace of the server (created when missing; "
		"default lmms-collab_<computer name>).", "name"};
	parser.addOption(portOption);
	parser.addOption(listenOption);
	parser.addOption(dataOption);
	for (const auto& option : {p4PortOption, p4UserOption, p4DepotOption, p4ClientOption}) { parser.addOption(option); }
	// Security: a password (also from the environment variable LMMS_COLLAB_PASSWORD) and encryption
	const QCommandLineOption passwordFileOption{"password-file", "File whose first line is the password everybody "
		"needs to connect (or set LMMS_COLLAB_PASSWORD).", "file"};
	const QCommandLineOption tlsCertOption{"tls-cert", "Encrypt connections (TLS) with this certificate (PEM).", "file"};
	const QCommandLineOption tlsKeyOption{"tls-key", "The certificate's private key (PEM).", "file"};
	for (const auto& option : {passwordFileOption, tlsCertOption, tlsKeyOption}) { parser.addOption(option); }
	parser.process(app);

	bool portOk = false;
	const uint port = parser.value(portOption).toUInt(&portOk);
	if (!portOk || port == 0 || port > 65535)
	{
		qCritical("Invalid port %s", qPrintable(parser.value(portOption)));
		return 1;
	}

	const QDir dataDir{parser.value(dataOption)};
	lmms::collab::CollabServer server{dataDir};
	if (parser.isSet(p4PortOption))
	{
		lmms::collab::P4Exporter::Config p4;
		p4.port = parser.value(p4PortOption);
		p4.user = parser.value(p4UserOption);
		p4.depot = parser.value(p4DepotOption);
		while (p4.depot.endsWith('/')) { p4.depot.chop(1); }
		p4.client = parser.isSet(p4ClientOption) ? parser.value(p4ClientOption)
			: "lmms-collab_" + QSysInfo::machineHostName().section('.', 0, 0);
		p4.root = QDir{dataDir.absoluteFilePath("p4")}.absolutePath();
		if (p4.user.isEmpty() || !lmms::collab::P4Exporter::isValidDepotPath(p4.depot) || p4.client.isEmpty())
		{
			qCritical("--p4port needs --p4user and --p4depot (like //depot/music)");
			return 1;
		}
		server.setPerforce(p4);
		qInfo("Versions are also submitted to Perforce %s as %s into %s (workspace %s)", qPrintable(p4.port),
			qPrintable(p4.user), qPrintable(p4.depot), qPrintable(p4.client));
	}
	QString password = qEnvironmentVariable("LMMS_COLLAB_PASSWORD");
	if (parser.isSet(passwordFileOption))
	{
		QFile file{parser.value(passwordFileOption)};
		if (!file.open(QIODevice::ReadOnly))
		{
			qCritical("Cannot read the password file %s", qPrintable(parser.value(passwordFileOption)));
			return 1;
		}
		password = QString::fromUtf8(file.readLine()).trimmed();
	}
	if (!password.isEmpty())
	{
		server.setPasswordKey(lmms::collab::proto::passwordKey(password));
		qInfo("A password is needed to connect");
	}
	else if (!QHostAddress{parser.value(listenOption)}.isLoopback())
	{
		qWarning("No password: anyone who can reach this server can join its projects (see --password-file)");
	}
	if (parser.isSet(tlsCertOption) || parser.isSet(tlsKeyOption))
	{
		if (!server.setTls(parser.value(tlsCertOption), parser.value(tlsKeyOption))) { return 1; }
	}
	if (!server.listen(parser.value(listenOption), static_cast<quint16>(port))) { return 1; }

	// Graceful shutdown on Ctrl+C / SIGTERM: leave the event loop, then the server saves every project
#ifdef _WIN32
	SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
	std::signal(SIGINT, requestQuit);
	std::signal(SIGTERM, requestQuit);
#endif
	QTimer quitPoll;
	QObject::connect(&quitPoll, &QTimer::timeout, &app, [] {
		if (s_quitRequested) { QCoreApplication::quit(); }
	});
	quitPoll.start(200);

	const int result = QCoreApplication::exec();
	server.saveAll();
	qInfo("Server stopped");
	s_shutdownComplete = true;
	return result;
}
