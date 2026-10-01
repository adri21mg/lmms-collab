/*
 * P4Exporter.cpp - submits versions of shared projects to Perforce (Helix Core)
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

#include "P4Exporter.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>

namespace lmms::collab
{

namespace
{
constexpr int TimeoutMs = 10 * 60 * 1000; // large shared files take a while
const QString SharedFolder = "Project files";
}


bool P4Exporter::isValidDepotPath(const QString& path)
{
	static const QRegularExpression re{"^//[A-Za-z0-9_.-]+(/[A-Za-z0-9 _.-]+)*$"};
	return re.match(path).hasMatch() && !path.contains("..");
}


QByteArray P4Exporter::depotMmp(const QByteArray& mmp)
{
	// "shared:" paths only resolve inside a collaboration session; "local:" is relative to the project file
	QByteArray result = mmp;
	result.replace("\"shared:", "\"local:" + SharedFolder.toUtf8() + "/");
	return result;
}


P4Exporter::Output P4Exporter::run(const Config& config, const QStringList& arguments, const QByteArray& input)
{
	QProcess p4;
	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.remove("P4CONFIG"); // only what is given here counts
	environment.remove("P4CLIENT");
	p4.setProcessEnvironment(environment);
	p4.setProcessChannelMode(QProcess::MergedChannels);
	p4.start(config.program, QStringList{"-s", "-p", config.port, "-u", config.user, "-c", config.client} + arguments);
	Output output;
	if (!p4.waitForStarted(10000))
	{
		output.errors.append("cannot run " + config.program + " (is the Perforce command-line client installed?)");
		return output;
	}
	if (!input.isEmpty()) { p4.write(input); }
	p4.closeWriteChannel();
	if (!p4.waitForFinished(TimeoutMs))
	{
		p4.kill();
		output.errors.append("p4 " + arguments.value(0) + " did not finish");
		return output;
	}
	for (const QString& line : QString::fromUtf8(p4.readAll()).split('\n'))
	{
		const QString text = line.trimmed();
		if (text.startsWith("error:")) { output.errors.append(text.mid(6).trimmed()); }
		else if (text.startsWith("info") || text.startsWith("text:")) { output.info.append(text.section(':', 1).trimmed()); }
	}
	output.ok = p4.exitStatus() == QProcess::NormalExit && p4.exitCode() == 0 && output.errors.isEmpty();
	return output;
}


P4Exporter::Result P4Exporter::submit(const Config& config, const Job& job)
{
	Result result;
	auto fail = [&result](const QString& what, const QStringList& errors) {
		result.status = Result::Status::Failed;
		result.error = what + (errors.isEmpty() ? QString{} : ": " + errors.join(' '));
		return result;
	};

	// The server's workspace: only the music folder, files always writable
	const QString spec = QString{"Client:\t%1\nOwner:\t%2\nRoot:\t%3\n"
		"Options:\tallwrite clobber nocompress unlocked nomodtime normdir\nSubmitOptions:\trevertunchanged\n"
		"LineEnd:\tlocal\nDescription:\n\tLMMS-Collab server: versions of shared projects\nView:\n\t\"%4/...\" \"//%1/...\"\n"}
		.arg(config.client, config.user, QDir::toNativeSeparators(config.root), config.depot);
	if (const Output out = run(config, {"client", "-i"}, spec.toUtf8()); !out.ok)
	{
		return fail("cannot set up the Perforce workspace", out.errors);
	}

	// What Perforce has now (the first time there is nothing)
	const QDir root{config.root};
	const QString projectDir = root.filePath(job.project);
	run(config, {"sync", "-q", root.filePath(".gdignore"), projectDir + "/..."});

	// The version, written over it: whatever differs is an edit, an addition or a deletion
	if (!QDir{}.mkpath(projectDir)) { return fail("cannot create " + projectDir, {}); }
	QFile gdignore{root.filePath(".gdignore")};
	if (!gdignore.exists() && !gdignore.open(QIODevice::WriteOnly)) { return fail("cannot write .gdignore", {}); }
	gdignore.close();
	QDir{projectDir}.removeRecursively();
	if (!QDir{}.mkpath(projectDir + "/" + SharedFolder)) { return fail("cannot create " + projectDir, {}); }
	QSaveFile mmp{projectDir + "/" + job.project + ".mmp"};
	if (!mmp.open(QIODevice::WriteOnly) || mmp.write(depotMmp(job.mmp)) < 0 || !mmp.commit())
	{
		return fail("cannot write the project into the workspace", {});
	}
	for (const auto& [name, source] : job.files)
	{
		if (!QFile::copy(source, projectDir + "/" + SharedFolder + "/" + name))
		{
			return fail("cannot copy " + name + " into the workspace", {});
		}
	}

	// A changelist with the user's words
	QString description;
	for (const QString& line : job.description.split('\n')) { description += "\t" + line + "\n"; }
	const Output created = run(config, {"change", "-i"},
		QString{"Change:\tnew\nClient:\t%1\nUser:\t%2\nStatus:\tnew\nDescription:\n%3"}
			.arg(config.client, config.user, description).toUtf8());
	static const QRegularExpression createdRe{"Change (\\d+) created"};
	const auto createdMatch = createdRe.match(created.info.join('\n'));
	if (!created.ok || !createdMatch.hasMatch()) { return fail("cannot create a changelist", created.errors); }
	const QString change = createdMatch.captured(1);
	auto discard = [&] {
		run(config, {"revert", "-c", change, "//" + config.client + "/..."});
		run(config, {"change", "-d", change});
	};

	run(config, {"reconcile", "-c", change, "-a", "-e", "-d", root.filePath(".gdignore"), projectDir + "/..."});
	const Output opened = run(config, {"opened", "-c", change});
	if (opened.info.isEmpty())
	{
		discard();
		result.status = Result::Status::Unchanged;
		return result;
	}
	const Output submitted = run(config, {"submit", "-c", change});
	static const QRegularExpression submittedRe{"Change (\\d+) (?:renamed change (\\d+) and )?submitted"};
	const auto submittedMatch = submittedRe.match(submitted.info.join('\n'));
	if (!submittedMatch.hasMatch())
	{
		discard();
		return fail("cannot submit", submitted.errors);
	}
	result.status = Result::Status::Submitted;
	result.change = (submittedMatch.captured(2).isEmpty() ? submittedMatch.captured(1) : submittedMatch.captured(2)).toInt();
	return result;
}

} // namespace lmms::collab
