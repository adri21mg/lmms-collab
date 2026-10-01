/*
 * P4Exporter.h - submits versions of shared projects to Perforce (Helix Core)
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

#ifndef LMMS_COLLAB_P4_EXPORTER_H
#define LMMS_COLLAB_P4_EXPORTER_H

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace lmms::collab
{

/**
 * The server is the only one that talks to Perforce: users never need a P4 setup to collaborate.
 * A version is written into the server's own workspace, which maps only the depot folder for music
 * (e.g. //depot/music/...):
 *   <depot folder>/.gdignore                         (Godot ignores the folder: not imported, not exported)
 *   <depot folder>/<project>/<project>.mmp           (plain XML; shared files as "local:Project files/...")
 *   <depot folder>/<project>/Project files/<file>    (the project's shared files)
 * and then reconciled and submitted with the user's description.
 */
class P4Exporter
{
public:
	struct Config
	{
		QString port;    //!< e.g. 127.0.0.1:1667
		QString user;    //!< logged in once on this computer (its ticket must not expire)
		QString depot;   //!< depot folder for music, e.g. //depot/music
		QString client;  //!< workspace of the server (created when missing)
		QString root;    //!< local folder of that workspace
		QString program = "p4";
	};

	struct Job
	{
		QString project;
		QByteArray mmp;
		QList<QPair<QString, QString>> files; //!< (name in "Project files", file on disk)
		QString description;
	};

	struct Result
	{
		enum class Status { Submitted, Unchanged, Failed } status = Status::Failed;
		int change = 0;
		QString error;
	};

	//! Writes the version into the workspace and submits it. Blocking: runs on a worker thread.
	static Result submit(const Config& config, const Job& job);

	//! The project as it is stored in Perforce: shared files are found next to it, so it opens anywhere
	static QByteArray depotMmp(const QByteArray& mmp);

	static bool isValidDepotPath(const QString& path);

private:
	struct Output
	{
		bool ok = false;
		QStringList info;
		QStringList errors;
	};
	//! Runs p4 in script mode (-s): every line says whether it is info, a warning or an error
	static Output run(const Config& config, const QStringList& arguments, const QByteArray& input = {});
};

} // namespace lmms::collab

#endif // LMMS_COLLAB_P4_EXPORTER_H
