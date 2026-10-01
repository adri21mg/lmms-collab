/*
 * CollabSessionAssets.cpp - shared files of a collaboration project (milestone M6a)
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

// Samples, SoundFonts and other files a project uses must exist on every client. LMMS' own files (its
// factory samples and presets) do; everything else is shared through the server: uploaded once, named
// "shared:<path>" in the project (a path prefix like LMMS' "factorysample:"), and kept by every client in
// its own copy of the project's library folder. Files are identified by their SHA-256, so the same content
// is never stored or sent twice.
//
// Before an object that uses a file only this computer has is sent (an instrument, a sample clip, a new
// track), the user is asked whether to share that file. Until the answer, the object waits; if the user does
// not share it, what uses it is undone, so nobody hears something the others cannot.

#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>

#include "AudioEngine.h"
#include "CollabSession.h"
#include "CollabSessionUtil.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "PathUtil.h"
#include "PatternStore.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"

namespace lmms::collab
{

namespace
{


QString fileHash(const QString& path)
{
	QFile file{path};
	QCryptographicHash sha{QCryptographicHash::Sha256};
	if (!file.open(QIODevice::ReadOnly) || !sha.addData(&file)) { return {}; }
	return QString::fromLatin1(sha.result().toHex());
}

QString megabytes(qint64 bytes)
{
	return QString::number(bytes / (1024.0 * 1024.0), 'f', bytes < 10 * 1024 * 1024 ? 1 : 0);
}

//! While a shared sample downloads, a silent placeholder stands in for it: LMMS loads it without an error and
//! keeps the file's name (a missing file would be forgotten), then the real one replaces it and is reloaded.
//! Audio files are recognized by their content, so a WAV works for every sample type.
void writePlaceholder(const QString& path)
{
	static const QRegularExpression sampleType{"\\.(wav|ogg|flac|mp3|aif|aiff|au|voc|w64)$", QRegularExpression::CaseInsensitiveOption};
	if (QFileInfo::exists(path) || !sampleType.match(path).hasMatch()) { return; }
	QByteArray wav;
	auto u32 = [&wav](quint32 v) { for (int i = 0; i < 4; ++i) { wav.append(static_cast<char>((v >> (8 * i)) & 0xff)); } };
	auto u16 = [&wav](quint16 v) { wav.append(static_cast<char>(v & 0xff)); wav.append(static_cast<char>(v >> 8)); };
	wav.append("RIFF"); u32(38); wav.append("WAVEfmt "); u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
	wav.append("data"); u32(2); u16(0);
	QFile file{path};
	if (file.open(QIODevice::WriteOnly)) { file.write(wav); }
}

bool samePath(const QString& a, const QString& b)
{
	return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

std::vector<Track*> allTracks()
{
	std::vector<Track*> tracks = Engine::getSong()->tracks();
	const auto& patternTracks = Engine::patternStore()->tracks();
	tracks.insert(tracks.end(), patternTracks.begin(), patternTracks.end());
	return tracks;
}

//! The instrument's own element (e.g. <audiofileprocessor src="...">), as it saves itself
QDomElement instrumentState(InstrumentTrack* track, QDomDocument& doc)
{
	QDomElement parent = doc.createElement("collab");
	doc.appendChild(parent);
	if (track->instrument()) { track->instrument()->saveState(doc, parent); }
	return parent.firstChildElement();
}

//! Sets every attribute of @p element (and below) that names @p absolutePath to @p value; true if any did
bool replaceFileRefs(QDomElement element, const QString& absolutePath, const QString& value)
{
	bool changed = false;
	const QDomNamedNodeMap attributes = element.attributes();
	for (int i = 0; i < attributes.size(); ++i)
	{
		QDomAttr attribute = attributes.item(i).toAttr();
		if (!attribute.value().isEmpty() && samePath(PathUtil::toAbsolute(attribute.value()), absolutePath))
		{
			attribute.setValue(value);
			changed = true;
		}
	}
	for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
	{
		changed |= replaceFileRefs(child, absolutePath, value);
	}
	return changed;
}

} // namespace


QString CollabSession::libraryDir() const
{
	return ConfigManager::inst()->workingDir() + "collab/" + m_project + "/library/";
}


void CollabSession::setLibrary(const QJsonArray& files)
{
	m_library.clear();
	QDir{}.mkpath(libraryDir());
	PathUtil::setSharedLocation(libraryDir());
	for (const QJsonValue& value : files)
	{
		const QJsonObject file = value.toObject();
		const QString path = proto::sanitizeAssetName(file.value("path").toString());
		const QString hash = file.value("hash").toString();
		if (path.isEmpty() || !proto::isValidHash(hash)) { continue; }
		m_library.insert(hash, SharedFile{path, file.value("size").toInteger()});
		if (QFileInfo{libraryDir() + path}.size() != file.value("size").toInteger()) { writePlaceholder(libraryDir() + path); }
	}
}


void CollabSession::addLibraryFile(const QString& path, const QString& hash, qint64 size, bool fromOthers)
{
	if (proto::sanitizeAssetName(path) != path || !proto::isValidHash(hash)) { return; }
	m_library.insert(hash, SharedFile{path, size});
	if (!fromOthers || QFileInfo{libraryDir() + path}.size() == size) { return; }
	log(QString{"shared file %1 (%2 bytes) added by a collaborator"}.arg(path).arg(size));
	// Everyone in the session has every shared file: it is downloaded right away
	writePlaceholder(libraryDir() + path);
	m_downloadQueue.append(hash);
	requestNextDownload();
}


QStringList CollabSession::missingFiles() const
{
	QStringList missing;
	for (auto it = m_library.cbegin(); it != m_library.cend(); ++it)
	{
		if (QFileInfo{libraryDir() + it->path}.size() != it->size && !m_downloadQueue.contains(it.key())) { missing.append(it.key()); }
	}
	return missing;
}


bool CollabSession::downloadMissing(const QStringList& missing)
{
	qint64 total = 0;
	for (const QString& hash : missing) { total += m_library.value(hash).size; }
	if (auto gui = gui::getGUI())
	{
		QMessageBox box{QMessageBox::Question, tr("Shared files"),
			tr("This project has %1 shared file(s) you do not have yet (%2 MB). They are needed to hear the project "
				"like everybody else.").arg(missing.size()).arg(megabytes(total)), QMessageBox::NoButton, gui->mainWindow()};
		QPushButton* download = box.addButton(tr("Download and join"), QMessageBox::AcceptRole);
		box.addButton(tr("Don't join"), QMessageBox::RejectRole);
		box.exec();
		if (box.clickedButton() != download) { return false; }
		m_downloadProgress = new QProgressDialog{tr("Downloading the project's shared files..."), tr("Leave"), 0, 1000,
			gui->mainWindow()};
		m_downloadProgress->setWindowTitle(tr("Shared files"));
		m_downloadProgress->setWindowModality(Qt::WindowModal);
		m_downloadProgress->setMinimumDuration(0);
		m_downloadProgress->setAttribute(Qt::WA_DeleteOnClose);
		connect(m_downloadProgress, &QProgressDialog::canceled, this, [this] {
			if (!m_downloading.isEmpty() || !m_downloadQueue.isEmpty()) { disconnectFromServer(); }
		});
		m_downloadTotal = total;
		m_downloadDone = 0;
	}
	m_downloadQueue.append(missing);
	requestNextDownload();
	return true;
}


void CollabSession::downloadsChanged()
{
	// Not right here: this runs while a message is being handled
	QTimer::singleShot(0, this, [this] {
		updateDownloadProgress();
		if (m_pendingJoin && m_downloading.isEmpty() && m_downloadQueue.isEmpty())
		{
			const QJsonObject joined = *m_pendingJoin; // finishJoin() forgets the waiting one
			finishJoin(joined);
		}
	});
}


void CollabSession::updateDownloadProgress()
{
	if (!m_downloadProgress) { return; }
	if (m_downloading.isEmpty() && m_downloadQueue.isEmpty())
	{
		m_downloadProgress->close();
		return;
	}
	const qint64 done = m_downloadDone + m_downloadReceived;
	m_downloadProgress->setValue(static_cast<int>(m_downloadTotal > 0 ? done * 1000 / m_downloadTotal : 0));
}


void CollabSession::requestNextDownload()
{
	if (!m_downloading.isEmpty() || m_downloadQueue.isEmpty() || !m_socket) { return; }
	m_downloading = m_downloadQueue.takeFirst();
	m_downloadFile.reset();
	log(QString{"requesting shared file %1"}.arg(m_library.value(m_downloading).path));
	send({{"t", proto::msg::AssetGet}, {"hash", m_downloading}});
}


void CollabSession::handleAssetMessage(const QJsonObject& message)
{
	const QString t = message.value("t").toString();
	const QString hash = message.value("hash").toString();
	log(QString{"%1 %2 (downloading %3)"}.arg(t, hash.left(12), m_downloading.left(12)));
	if (t == proto::msg::AssetData && hash == m_downloading && m_library.contains(hash))
	{
		m_downloadSize = message.value("size").toInteger();
		m_downloadReceived = 0;
		m_downloadFile = std::make_unique<QFile>(libraryDir() + ".part-" + hash);
		if (!m_downloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			log("cannot write " + m_downloadFile->fileName());
			m_downloadFile.reset();
			m_downloading.clear();
			requestNextDownload();
		}
	}
	else if (t == proto::msg::AssetStored && m_uploads.contains(hash))
	{
		// Our file is shared: everyone learns about it, then what uses it here uses the shared copy
		const QString local = m_uploads.take(hash);
		const QString path = proto::sanitizeAssetName(message.value("path").toString());
		if (path.isEmpty()) { return; }
		const qint64 size = QFileInfo{local}.size();
		if (!QFileInfo::exists(libraryDir() + path)) { QFile::copy(local, libraryDir() + path); }
		m_library.insert(hash, SharedFile{path, size});
		sendOps({QJsonObject{{"op", proto::op::LibraryAdd}, {"path", path}, {"hash", hash}, {"size", size}}});
		log(QString{"shared %1 as %2"}.arg(local, path));
		rewriteReferences(local, path);
	}
	else if (t == proto::msg::AssetError)
	{
		log(QString{"file transfer %1 failed: %2"}.arg(hash, message.value("message").toString()));
		if (m_uploads.contains(hash))
		{
			const QString local = m_uploads.take(hash);
			if (auto gui = gui::getGUI())
			{
				QMessageBox::warning(gui->mainWindow(), tr("Shared file"), tr("\"%1\" could not be shared: %2")
					.arg(QFileInfo{local}.fileName(), message.value("message").toString()));
			}
			dropReferences(local);
		}
		if (hash == m_downloading)
		{
			m_downloadFile.reset();
			m_downloading.clear();
			requestNextDownload();
			downloadsChanged();
		}
	}
}


void CollabSession::handleBinary(const QByteArray& payload)
{
	if (payload.size() < proto::AssetHashSize || !m_downloadFile) { return; }
	const QString hash = QString::fromLatin1(payload.left(proto::AssetHashSize).toHex());
	if (hash != m_downloading) { return; }
	const QByteArray data = payload.mid(proto::AssetHashSize);
	m_downloadFile->write(data);
	m_downloadReceived += data.size();
	if (m_downloadReceived < m_downloadSize)
	{
		downloadsChanged();
		return;
	}

	// Complete: keep it only if the content is exactly the shared one
	const QString part = m_downloadFile->fileName();
	m_downloadFile->close();
	m_downloadFile.reset();
	const SharedFile file = m_library.value(hash);
	if (fileHash(part) == hash)
	{
		QFile::remove(libraryDir() + file.path);
		QFile::rename(part, libraryDir() + file.path);
		log(QString{"downloaded shared file %1"}.arg(file.path));
		if (m_state == State::Live) { reloadReferences(file.path); } // before joining, nothing uses it yet
	}
	else
	{
		log(QString{"shared file %1 arrived damaged"}.arg(file.path));
		QFile::remove(part);
	}
	m_downloadDone += m_downloadSize;
	m_downloadReceived = 0;
	m_downloading.clear();
	requestNextDownload();
	downloadsChanged();
}


QHash<QString, QString> CollabSession::localFileRefs(const QString& xml)
{
	static const QRegularExpression fileType{
		"\\.(wav|ogg|flac|mp3|aif|aiff|au|voc|w64|raw|sf2|sf3|gig|pat|ds|xiz|mid|midi)$",
		QRegularExpression::CaseInsensitiveOption};
	QHash<QString, QString> refs;
	QDomDocument doc;
	if (!doc.setContent(xml)) { return refs; }
	std::function<void(const QDomElement&)> visit = [&](const QDomElement& element) {
		const QDomNamedNodeMap attributes = element.attributes();
		for (int i = 0; i < attributes.size(); ++i)
		{
			const QString value = attributes.item(i).toAttr().value();
			if (value.size() < 5 || !fileType.match(value).hasMatch()) { continue; }
			// LMMS' own files exist everywhere; shared ones are shared already
			const auto base = PathUtil::baseLookup(value);
			if (base == PathUtil::Base::FactorySample || base == PathUtil::Base::FactoryPresets
				|| base == PathUtil::Base::FactoryProjects || base == PathUtil::Base::Shared)
			{
				continue;
			}
			const QString absolute = PathUtil::toAbsolute(value);
			if (QFileInfo{absolute}.isFile()) { refs.insert(value, absolute); }
		}
		for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) { visit(child); }
	};
	visit(doc.documentElement());
	return refs;
}


bool CollabSession::readyToSend(const QString& xml)
{
	const QHash<QString, QString> refs = localFileRefs(xml);
	for (const QString& absolute : refs)
	{
		// Already on its way (being asked about, uploading, queued): once is enough
		if (m_askingAbout.contains(absolute) || m_uploads.values().contains(absolute) || m_shareQueue.contains(absolute)) { continue; }
		m_shareQueue.append(absolute);
	}
	if (!refs.isEmpty() && !m_shareScheduled)
	{
		// Not in the middle of sending: sharing may ask the user and changes instruments and clips
		m_shareScheduled = true;
		QTimer::singleShot(0, this, [this] {
			m_shareScheduled = false;
			processShareQueue();
		});
	}
	return refs.isEmpty();
}


void CollabSession::processShareQueue()
{
	if (m_state != State::Live || m_asking || m_shareQueue.isEmpty()) { return; }

	// Files the project already shares (the same content) are used right away
	QStringList unknown;
	qint64 total = 0;
	for (const QString& local : m_shareQueue)
	{
		QString hash = m_localHashes.value(local);
		if (hash.isEmpty()) { m_localHashes.insert(local, hash = fileHash(local)); }
		if (const auto shared = m_library.constFind(hash); shared != m_library.cend())
		{
			if (!QFileInfo::exists(libraryDir() + shared->path)) { QFile::copy(local, libraryDir() + shared->path); }
			rewriteReferences(local, shared->path);
		}
		else
		{
			unknown.append(local);
			total += QFileInfo{local}.size();
		}
	}
	m_shareQueue.clear();
	if (unknown.isEmpty()) { return; }

	QStringList names;
	for (const QString& local : unknown)
	{
		names.append(QFileInfo{local}.fileName());
		m_askingAbout.insert(local);
	}
	bool share = true;
	if (auto gui = gui::getGUI())
	{
		m_asking = true;
		share = QMessageBox::question(gui->mainWindow(), tr("Share with the project?"),
			tr("These files are only on your computer:\n\n%1\n\nShare them with the project (%2 MB), so the others hear "
				"them too? If not, they are not used.").arg(names.join('\n'), megabytes(total)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes;
		m_asking = false;
	}
	for (const QString& local : unknown)
	{
		// Not shared: what uses it is undone (if it is used again later, the user is asked again)
		if (share && QFileInfo{local}.size() <= proto::MaxAssetSize) { uploadFile(local); }
		else { dropReferences(local); }
	}
	m_askingAbout.clear();
	// Files queued while the user was answering
	if (!m_shareQueue.isEmpty()) { QTimer::singleShot(0, this, [this] { processShareQueue(); }); }
}


void CollabSession::uploadFile(const QString& absolutePath)
{
	QFile file{absolutePath};
	const QString hash = m_localHashes.value(absolutePath, fileHash(absolutePath));
	if (!m_socket || hash.isEmpty() || m_uploads.contains(hash) || !file.open(QIODevice::ReadOnly)) { return; }
	m_uploads.insert(hash, absolutePath);
	send({{"t", proto::msg::AssetPut}, {"hash", hash}, {"size", file.size()}, {"name", QFileInfo{absolutePath}.fileName()}});
	const QByteArray rawHash = QByteArray::fromHex(hash.toLatin1());
	while (!file.atEnd()) { m_socket->write(proto::encodeBinaryFrame(rawHash, file.read(proto::AssetChunkSize))); }
	log(QString{"uploading %1 (%2 bytes)"}.arg(absolutePath).arg(file.size()));
}


void CollabSession::rewriteReferences(const QString& absoluteLocal, const QString& sharedPath)
{
	const QString value = PathUtil::basePrefix(PathUtil::Base::Shared) + sharedPath;
	const bool journalling = Engine::projectJournal()->isJournalling();
	Engine::projectJournal()->setJournalling(false);
	for (Track* track : allTracks())
	{
		if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track); instrumentTrack && instrumentTrack->instrument())
		{
			QDomDocument doc;
			QDomElement state = instrumentState(instrumentTrack, doc);
			if (!replaceFileRefs(state, absoluteLocal, value)) { continue; }
			{
				auto guard = Engine::audioEngine()->requestChangesGuard();
				instrumentTrack->instrument()->restoreState(state);
			}
			m_forcePluginCheck.insert(track->collabId()); // sent at the next flush (if it is shared already)
		}
		for (Clip* clip : track->getClips())
		{
			// Through its XML: setSampleFile() would also reset its length and offset
			QDomDocument doc;
			if (dynamic_cast<SampleClip*>(clip) && doc.setContent(serialize(clip))
				&& replaceFileRefs(doc.documentElement(), absoluteLocal, value))
			{
				clip->restoreState(doc.documentElement());
			}
		}
	}
	Engine::projectJournal()->setJournalling(journalling);
}


void CollabSession::dropReferences(const QString& absoluteLocal)
{
	log("not sharing " + absoluteLocal);
	for (Track* track : allTracks())
	{
		const collab_id_t id = track->collabId();
		if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track); instrumentTrack && instrumentTrack->instrument())
		{
			QDomDocument doc;
			if (localFileRefs(elementToString(instrumentState(instrumentTrack, doc))).values().contains(absoluteLocal))
			{
				if (!m_structure.tracks.contains(id))
				{
					removeTrack(track); // e.g. made by dropping the file on the Song Editor
					continue;
				}
				// Back to what the others have
				const QString synced = m_plugins.value(id).instrumentXml;
				if (!synced.isEmpty())
				{
					applyRemoteInstrument(QJsonObject{{"track", proto::idString(id)}, {"xml", synced}});
				}
			}
		}
		for (Clip* clip : track->getClips())
		{
			auto sampleClip = dynamic_cast<SampleClip*>(clip);
			if (!sampleClip || !samePath(PathUtil::toAbsolute(sampleClip->sampleFile()), absoluteLocal)) { continue; }
			const auto synced = m_structure.clips.constFind(clip->collabId());
			if (synced == m_structure.clips.cend()) { removeClip(clip); }
			else { sampleClip->setSampleFile(synced->fields.value("src").toString()); }
		}
	}
}


void CollabSession::reloadReferences(const QString& sharedPath)
{
	const QString value = PathUtil::basePrefix(PathUtil::Base::Shared) + sharedPath;
	const QString absolute = libraryDir() + sharedPath;
	const bool journalling = Engine::projectJournal()->isJournalling();
	Engine::projectJournal()->setJournalling(false);
	m_applyingRemote = true;
	for (Track* track : allTracks())
	{
		if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track); instrumentTrack && instrumentTrack->instrument())
		{
			QDomDocument doc;
			QDomElement state = instrumentState(instrumentTrack, doc);
			// Only what refers to this file (it may have been loaded as empty while the file was missing)
			if (replaceFileRefs(state, absolute, value))
			{
				auto guard = Engine::audioEngine()->requestChangesGuard();
				instrumentTrack->instrument()->restoreState(state);
			}
		}
		for (Clip* clip : track->getClips())
		{
			QDomDocument doc;
			if (dynamic_cast<SampleClip*>(clip) && doc.setContent(serialize(clip))
				&& replaceFileRefs(doc.documentElement(), absolute, value))
			{
				clip->restoreState(doc.documentElement());
			}
		}
	}
	m_applyingRemote = false;
	Engine::projectJournal()->setJournalling(journalling);
	m_structure = currentStructure(); // reloading is not a change of anyone
	for (Track* track : allTracks())
	{
		if (m_plugins.contains(track->collabId())) { rebasePlugins(track); }
	}
}


void CollabSession::shareProjectFiles()
{
	// The song as it was shared may use files only this computer has
	for (Track* track : allTracks())
	{
		if (auto instrumentTrack = dynamic_cast<InstrumentTrack*>(track); instrumentTrack && instrumentTrack->instrument())
		{
			QDomDocument doc;
			readyToSend(elementToString(instrumentState(instrumentTrack, doc)));
		}
		for (Clip* clip : track->getClips())
		{
			if (auto sampleClip = dynamic_cast<SampleClip*>(clip)) { readyToSend(serialize(sampleClip)); }
		}
	}
}

} // namespace lmms::collab
