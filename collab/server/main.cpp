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
	parser.addOption(portOption);
	parser.addOption(listenOption);
	parser.addOption(dataOption);
	parser.process(app);

	bool portOk = false;
	const uint port = parser.value(portOption).toUInt(&portOk);
	if (!portOk || port == 0 || port > 65535)
	{
		qCritical("Invalid port %s", qPrintable(parser.value(portOption)));
		return 1;
	}

	lmms::collab::CollabServer server{QDir{parser.value(dataOption)}};
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
