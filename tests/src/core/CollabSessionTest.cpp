/*
 * CollabSessionTest.cpp - end-to-end test: LMMS model <-> collaboration server <-> fake collaborator
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

#include <QCryptographicHash>
#include <QDomDocument>
#include <QJsonArray>
#include <QTextStream>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "AudioBusHandle.h"
#include "CollabProtocol.h"
#include "CollabSession.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "Effect.h"
#include "EffectChain.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Mixer.h"
#include "Note.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PathUtil.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"

using namespace lmms;
using namespace lmms::collab;

//! Minimal second collaborator speaking the wire protocol directly
class FakePeer
{
public:
	bool connectTo(quint16 port)
	{
		m_socket.connectToHost("127.0.0.1", port);
		if (!m_socket.waitForConnected(3000)) { return false; }
		send({{"t", "hello"}, {"proto", proto::Version}, {"user", "Yeray"}});
		const auto welcome = next("welcome");
		m_clientId = welcome.value("clientId").toString();
		return !m_clientId.isEmpty();
	}

	void send(const QJsonObject& m) { m_socket.write(proto::encodeJsonFrame(m)); m_socket.flush(); }

	void sendOps(const QJsonArray& ops) { send({{"t", "tx"}, {"ctx", ++m_ctx}, {"ops", ops}}); }

	//! Next message (optionally of a given type), waiting up to @p timeoutMs; empty object on timeout
	QJsonObject next(const QString& type = {}, int timeoutMs = 3000)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < timeoutMs)
		{
			proto::FrameType t;
			QByteArray payload;
			while (m_decoder.next(t, payload))
			{
				if (t == proto::FrameType::Binary)
				{
					m_binary.append(payload);
					continue;
				}
				const auto msg = proto::parseMessage(payload);
				if (msg && (type.isEmpty() || msg->value("t").toString() == type)) { return *msg; }
			}
			// Keep the LMMS side (same thread) running while we wait for data
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
			if (m_socket.bytesAvailable() > 0 || m_socket.waitForReadyRead(5))
			{
				m_decoder.append(m_socket.readAll());
			}
		}
		return {};
	}

	//! Next transaction sent by someone other than us
	QJsonObject nextForeignTx(int timeoutMs = 3000)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < timeoutMs)
		{
			const auto tx = next("tx", timeoutMs - static_cast<int>(timer.elapsed()));
			if (tx.isEmpty()) { break; }
			if (tx.value("clientId").toString() != m_clientId) { return tx; }
		}
		return {};
	}

	//! Next operation of type @p opType sent by someone else (other ops, e.g. derived clip lengths, are skipped)
	QJsonObject nextForeignOp(const QString& opType, int timeoutMs = 3000)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < timeoutMs)
		{
			if (!m_ops.isEmpty())
			{
				const QJsonObject op = m_ops.takeFirst();
				if (op.value("op").toString() == opType) { return op; }
				continue;
			}
			const auto tx = nextForeignTx(timeoutMs - static_cast<int>(timer.elapsed()));
			if (tx.isEmpty()) { break; }
			for (const auto& op : tx.value("ops").toArray()) { m_ops.append(op.toObject()); }
		}
		return {};
	}

	//! Forgets everything received so far
	void drain()
	{
		m_ops.clear();
		while (!next({}, 200).isEmpty()) {}
	}

	QString clientId() const { return m_clientId; }

	//! Downloads a shared file (asset.get); empty on failure
	QByteArray download(const QString& hash)
	{
		m_binary.clear();
		send({{"t", "asset.get"}, {"hash", hash}});
		const auto header = next("asset.data");
		const qint64 size = header.value("size").toInteger();
		QByteArray data;
		QElapsedTimer timer;
		timer.start();
		while (data.size() < size && timer.elapsed() < 5000)
		{
			while (!m_binary.isEmpty()) { data.append(m_binary.takeFirst().mid(proto::AssetHashSize)); }
			if (data.size() < size) { next("__none__", 20); }
		}
		return data;
	}

	//! Uploads a shared file; returns the path the server gave it
	QString upload(const QByteArray& data, const QString& name)
	{
		const QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
		send({{"t", "asset.put"}, {"hash", QString::fromLatin1(hash.toHex())}, {"size", data.size()}, {"name", name}});
		m_socket.write(proto::encodeBinaryFrame(hash, data));
		m_socket.flush();
		return next("asset.stored").value("path").toString();
	}

private:
	QList<QByteArray> m_binary;
	QTcpSocket m_socket;
	proto::FrameDecoder m_decoder;
	QString m_clientId;
	qint64 m_ctx = 0;
	QList<QJsonObject> m_ops;
};


class CollabSessionTest : public QObject
{
	Q_OBJECT
private:
	QTemporaryDir m_dataDir;
	QProcess m_server;
	quint16 m_port = 0;
	MidiClip* m_clip = nullptr;
	FakePeer m_peer;

	static QJsonObject noteOp(const char* op, MidiClip* clip, collab_id_t id, const QJsonObject& v = {})
	{
		QJsonObject o{{"op", op}, {"clip", proto::idString(clip->collabId())}, {"id", proto::idString(id)}};
		if (!v.isEmpty()) { o.insert("v", v); }
		return o;
	}

	//! Lets queued network traffic and throttle timers run
	static void settle(int ms = 300)
	{
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); }
	}

private slots:
	void initTestCase()
	{
		// No plugins are loaded here (they link against lmms.exe): instruments and effects are LMMS' dummies,
		// which is enough to test how they are synchronized; real plugins are tested in the application
		Engine::init(true);
		QVERIFY(m_dataDir.isValid());
		// Never write into the user's real LMMS workspace (the session log lives in <workspace>/collab)
		ConfigManager::inst()->setWorkingDir(m_dataDir.filePath("workspace"));
		m_port = static_cast<quint16>(43000 + QRandomGenerator::global()->bounded(1000));
		m_server.setProcessChannelMode(QProcess::ForwardedChannels);
		m_server.start(COLLAB_SERVER_EXE, {"--port", QString::number(m_port), "--data", m_dataDir.path()});
		QVERIFY(m_server.waitForStarted(5000));
		QTest::qWait(500);

		auto track = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		m_clip = new MidiClip(track);
		m_clip->addNote(Note{TimePos{48}, TimePos{0}, 60}, false);

		// Like LMMS' default project: one pattern with one Pattern Editor track
		Track::create(Track::Type::Pattern, Engine::getSong());
		Track::create(Track::Type::Instrument, Engine::patternStore());
	}

	void cleanupTestCase()
	{
		CollabSession::instance()->disconnectFromServer();
		m_server.kill();
		m_server.waitForFinished(3000);
		Engine::destroy();
	}

	void testShareAndOpen()
	{
		auto session = CollabSession::instance();
		session->connectToServer("127.0.0.1", m_port, "Adri", "session-test", CollabSession::JoinMode::Create);
		QTRY_COMPARE_WITH_TIMEOUT(session->state(), CollabSession::State::Live, 5000);

		QVERIFY(m_peer.connectTo(m_port));
		m_peer.send({{"t", "open"}, {"project", "session-test"}});
		const auto joined = m_peer.next("joined");
		QDomDocument doc;
		QVERIFY(static_cast<bool>(doc.setContent(joined.value("mmp").toString())));
		const QString noteId = proto::idString(m_clip->notes().front()->collabId());
		bool found = false;
		const auto notes = doc.elementsByTagName("note");
		for (int i = 0; i < notes.size(); ++i)
		{
			found |= notes.at(i).toElement().attribute("cid") == noteId;
		}
		QVERIFY2(found, "the peer's snapshot contains our note with the same id");
	}

	void testLocalEditsReachPeer()
	{
		m_peer.drain();
		// add
		Note* note = m_clip->addNote(Note{TimePos{48}, TimePos{96}, 64}, false);
		auto op = m_peer.nextForeignOp("note.add");
		QCOMPARE(proto::parseId(op.value("id")), note->collabId());
		QCOMPARE(op.value("v").toObject().value("pos").toInt(), 96);
		QCOMPARE(op.value("v").toObject().value("key").toInt(), 64);

		// move (only changed fields are sent)
		note->setPos(TimePos{144});
		note->setKey(67);
		emit m_clip->dataChanged();
		op = m_peer.nextForeignOp("note.set");
		QCOMPARE(op.value("v").toObject(), (QJsonObject{{"pos", 144}, {"key", 67}}));

		// resize
		note->setLength(TimePos{96});
		emit m_clip->dataChanged();
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"len", 96}}));

		// delete
		const collab_id_t id = note->collabId();
		m_clip->removeNote(note);
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("note.remove").value("id")), id);
	}

	void testRemoteEditsApplyWithoutEcho()
	{
		m_peer.drain();
		const collab_id_t id = 0x00000000c0ffee01ULL;
		m_peer.sendOps({noteOp("note.add", m_clip, id,
			{{"key", 72}, {"pos", 48}, {"len", 24}, {"vol", 80}, {"pan", -10}, {"type", 0}})});
		QTRY_VERIFY(m_clip->findNote(id) != nullptr);
		Note* note = m_clip->findNote(id);
		QCOMPARE(note->key(), 72);
		QCOMPARE(note->getVolume(), 80);
		QCOMPARE(note->getPanning(), -10);

		m_peer.sendOps({noteOp("note.set", m_clip, id, {{"pos", 240}, {"len", 72}})});
		QTRY_COMPARE(m_clip->findNote(id)->pos().getTicks(), 240);
		QCOMPARE(m_clip->findNote(id)->length().getTicks(), 72);
		// notes stay sorted for playback
		QVERIFY(std::is_sorted(m_clip->notes().begin(), m_clip->notes().end(), Note::lessThan));

		m_peer.sendOps({noteOp("note.remove", m_clip, id)});
		QTRY_VERIFY(m_clip->findNote(id) == nullptr);

		// Applying remote changes must never send them back
		QVERIFY2(m_peer.nextForeignTx(500).isEmpty(), "no echo of remote changes");
	}

	void testConcurrentEditsConverge()
	{
		Note* note = m_clip->notes().front();
		const collab_id_t id = note->collabId();
		auto rng = QRandomGenerator::global();

		for (int round = 0; round < 40; ++round)
		{
			const int localKey = 20 + 2 * rng->bounded(40);  // even keys: ours
			const int remoteKey = 21 + 2 * rng->bounded(40); // odd keys: the peer's
			const bool localFirst = rng->bounded(2) == 0;
			const int gap = rng->bounded(80);

			auto local = [&] { m_clip->findNote(id)->setKey(localKey); emit m_clip->dataChanged(); };
			auto remote = [&] { m_peer.sendOps({noteOp("note.set", m_clip, id, {{"key", remoteKey}})}); };
			if (localFirst) { local(); settle(gap); remote(); }
			else { remote(); settle(gap); local(); }

			// The peer sees every transaction in server order; the last key it sees is the truth
			int truth = -1;
			QElapsedTimer quiet;
			quiet.start();
			while (quiet.elapsed() < 400)
			{
				const auto tx = m_peer.next("tx", 50);
				if (tx.isEmpty()) { continue; }
				for (const auto& o : tx.value("ops").toArray())
				{
					const auto v = o.toObject().value("v").toObject();
					if (v.contains("key")) { truth = v.value("key").toInt(); }
				}
				quiet.restart();
			}
			settle(100);
			QVERIFY2(truth != -1, "server ordered at least one edit");
			QCOMPARE(m_clip->findNote(id)->key(), truth);
		}
	}

private:
	//! Local edit gesture as an editor performs it: journal checkpoint first, then the change
	template<class F> void localGesture(F&& edit)
	{
		m_clip->addJournalCheckPoint();
		edit();
		emit m_clip->dataChanged();
		QVERIFY(!m_peer.nextForeignTx().isEmpty());
		m_peer.drain();
	}

	void remoteEdit(const QJsonObject& op)
	{
		m_peer.sendOps({op});
		settle(200);
		m_peer.drain();
	}

private slots:
	void testUndoOnlyRevertsOwnNotes()
	{
		auto journal = Engine::projectJournal();
		journal->setJournalling(true);

		collab_id_t mine = 0;
		localGesture([&] { mine = m_clip->addNote(Note{TimePos{48}, TimePos{384}, 50}, false)->collabId(); });
		const collab_id_t theirs = 0x00000000beef0001ULL;
		remoteEdit(noteOp("note.add", m_clip, theirs,
			{{"key", 52}, {"pos", 480}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}}));
		QVERIFY(m_clip->findNote(theirs));

		// Ctrl+Z removes my note but keeps the note Yeray added afterwards
		journal->undo();
		QVERIFY(m_clip->findNote(mine) == nullptr);
		QVERIFY(m_clip->findNote(theirs) != nullptr);
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("note.remove").value("id")), mine);

		// Ctrl+Y brings it back with the same identity
		journal->redo();
		QVERIFY(m_clip->findNote(mine) != nullptr);
		QCOMPARE(m_clip->findNote(mine)->key(), 50);
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("note.add").value("id")), mine);
		QVERIFY(m_clip->findNote(theirs) != nullptr);
	}

	void testUndoNeverOverwritesLaterChanges()
	{
		auto journal = Engine::projectJournal();
		const collab_id_t theirs = 0x00000000beef0001ULL;
		Note* note = m_clip->notes().front();
		const collab_id_t id = note->collabId();
		const int oldKey = note->key();
		const int oldPos = note->pos().getTicks();

		// I move the note, then Yeray moves it again: my Ctrl+Z must leave his change alone
		localGesture([&] { m_clip->findNote(id)->setKey(oldKey + 1); });
		remoteEdit(noteOp("note.set", m_clip, id, {{"key", oldKey + 5}}));
		journal->undo();
		QCOMPARE(m_clip->findNote(id)->key(), oldKey + 5);
		QVERIFY2(m_peer.nextForeignTx(400).isEmpty(), "nothing to revert, nothing sent");

		// Field by field: my position change is reverted, Yeray's volume change stays
		localGesture([&] { m_clip->findNote(id)->setPos(TimePos{oldPos + 48}); });
		remoteEdit(noteOp("note.set", m_clip, id, {{"vol", 42}}));
		journal->undo();
		QCOMPARE(m_clip->findNote(id)->pos().getTicks(), oldPos);
		QCOMPARE(m_clip->findNote(id)->getVolume(), 42);
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"pos", oldPos}}));

		// Undoing a whole-track checkpoint never resurrects old notes or drops newer remote ones
		m_clip->getTrack()->addJournalCheckPoint();
		const collab_id_t later = 0x00000000beef0002ULL;
		remoteEdit(noteOp("note.add", m_clip, later,
			{{"key", 55}, {"pos", 600}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}}));
		remoteEdit(noteOp("note.remove", m_clip, theirs));
		journal->undo();
		settle(200);
		QVERIFY2(m_clip->findNote(later) != nullptr, "remote note added after the checkpoint is kept");
		QVERIFY2(m_clip->findNote(theirs) == nullptr, "remote removal after the checkpoint is kept");

		// ...and the clip is still shared
		m_peer.drain();
		m_clip->findNote(id)->setKey(33);
		emit m_clip->dataChanged();
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"key", 33}}));
	}

	// ---- M2a: Song Editor structure ----

	void testLocalStructureReachesPeer()
	{
		m_peer.drain();
		auto song = Engine::getSong();

		// New track with a clip and a note: sent complete
		auto track = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song));
		auto clip = new MidiClip(track);
		clip->movePosition(TimePos{192});
		Note* note = clip->addNote(Note{TimePos{48}, TimePos{0}, 62}, false);
		auto op = m_peer.nextForeignOp("track.add");
		QDomDocument doc;
		QVERIFY(static_cast<bool>(doc.setContent(op.value("xml").toString())));
		QCOMPARE(proto::parseId(doc.documentElement().attribute("cid")), track->collabId());
		QVERIFY2(op.value("xml").toString().contains(proto::idString(clip->collabId())), "the clip travels with its track");
		QVERIFY2(op.value("xml").toString().contains(proto::idString(note->collabId())), "and its notes");
		m_newTrackXml = op.value("xml").toString();

		// Its notes are shared from now on
		note->setKey(63);
		emit clip->dataChanged();
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"key", 63}}));

		// Clip move, rename, mute and color
		clip->movePosition(TimePos{384});
		QCOMPARE(m_peer.nextForeignOp("clip.set").value("v").toObject(), (QJsonObject{{"pos", 384}}));
		clip->setName("Riff");
		clip->toggleMute();
		clip->setColor(QColor{"#ff0000"});
		QCOMPARE(m_peer.nextForeignOp("clip.set").value("v").toObject(),
			(QJsonObject{{"name", "Riff"}, {"muted", true}, {"color", "#ff0000"}}));

		// Track rename and color
		track->setName("Lead");
		QCOMPARE(m_peer.nextForeignOp("track.set").value("v").toObject(), (QJsonObject{{"name", "Lead"}}));

		// New clip, then remove it
		auto second = new MidiClip(track);
		second->movePosition(TimePos{768});
		op = m_peer.nextForeignOp("clip.add");
		QCOMPARE(proto::parseId(op.value("track")), track->collabId());
		const collab_id_t secondId = second->collabId();
		delete second;
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("clip.remove").value("id")), secondId);

		// Remove the track
		const collab_id_t trackId = track->collabId();
		delete track;
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("track.remove").value("id")), trackId);
	}

	void testRemoteStructureApplies()
	{
		m_peer.drain();
		auto song = Engine::getSong();

		// A track created by the peer (reuse the XML format LMMS produced, with fresh ids)
		QString xml = m_newTrackXml;
		QDomDocument doc;
		QVERIFY(static_cast<bool>(doc.setContent(xml)));
		const collab_id_t trackId = 0x00000000aaaa0001ULL;
		const collab_id_t clipId = 0x00000000aaaa0002ULL;
		QDomElement root = doc.documentElement();
		root.setAttribute("cid", proto::idString(trackId));
		root.setAttribute("name", "From Yeray");
		root.firstChildElement("midiclip").setAttribute("cid", proto::idString(clipId));
		QString text;
		QTextStream ts{&text};
		root.save(ts, 0);
		m_peer.sendOps({QJsonObject{{"op", "track.add"}, {"container", "song"}, {"index", -1}, {"xml", text}}});
		QTRY_VERIFY(findTrack(trackId) != nullptr);
		QCOMPARE(findTrack(trackId)->name(), QString{"From Yeray"});
		QTRY_VERIFY(findClip(clipId) != nullptr);

		// Remote notes in the remote clip
		auto clip = dynamic_cast<MidiClip*>(findClip(clipId));
		const collab_id_t noteId = 0x00000000aaaa0003ULL;
		m_peer.sendOps({noteOp("note.add", clip, noteId,
			{{"key", 40}, {"pos", 0}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}})});
		QTRY_VERIFY(clip->findNote(noteId) != nullptr);

		// Clip changes, track rename
		m_peer.sendOps({QJsonObject{{"op", "clip.set"}, {"id", proto::idString(clipId)},
			{"v", QJsonObject{{"pos", 960}, {"name", "Bass"}}}}});
		QTRY_COMPARE(clip->startPosition().getTicks(), 960);
		QCOMPARE(clip->name(), QString{"Bass"});
		m_peer.sendOps({QJsonObject{{"op", "track.set"}, {"id", proto::idString(trackId)},
			{"v", QJsonObject{{"name", "Bass line"}}}}});
		QTRY_COMPARE(findTrack(trackId)->name(), QString{"Bass line"});

		// Order: move the remote track to the front of the shared tracks
		QJsonArray ids{proto::idString(trackId)};
		for (Track* t : song->tracks())
		{
			if (t->collabId() != trackId && t->type() != Track::Type::Pattern) { ids.append(proto::idString(t->collabId())); }
		}
		m_peer.sendOps({QJsonObject{{"op", "track.order"}, {"container", "song"}, {"ids", ids}}});
		QTRY_VERIFY(std::find_if(song->tracks().begin(), song->tracks().end(),
			[](Track* t) { return t->type() != Track::Type::Pattern; }) != song->tracks().end()
			&& (*std::find_if(song->tracks().begin(), song->tracks().end(),
			[](Track* t) { return t->type() != Track::Type::Pattern; }))->collabId() == trackId);

		QVERIFY2(m_peer.nextForeignTx(500).isEmpty(), "no echo of remote structure changes");

		// Remote removal of the clip and the track
		m_peer.sendOps({QJsonObject{{"op", "clip.remove"}, {"id", proto::idString(clipId)}}});
		QTRY_VERIFY(findClip(clipId) == nullptr);
		m_peer.sendOps({QJsonObject{{"op", "track.remove"}, {"id", proto::idString(trackId)}}});
		QTRY_VERIFY(findTrack(trackId) == nullptr);
		QVERIFY2(m_peer.nextForeignTx(500).isEmpty(), "no echo of remote removals");
	}

	void testSoloStaysPrivate()
	{
		m_peer.drain();
		auto song = Engine::getSong();
		auto a = Track::create(Track::Type::Instrument, song);
		auto b = Track::create(Track::Type::Instrument, song);
		m_peer.nextForeignOp("track.add");
		m_peer.nextForeignOp("track.add");
		m_peer.drain();

		// My solo mutes other tracks locally, but that must not reach the peer
		a->setSolo(true);
		a->toggleSolo(); // what the track's view does when its solo button changes
		QVERIFY(b->isMuted());
		QVERIFY2(m_peer.nextForeignOp("track.set", 500).isEmpty(), "solo is private");

		// The peer mutes b meanwhile: it becomes b's mute after my solo
		m_peer.sendOps({QJsonObject{{"op", "track.set"}, {"id", proto::idString(b->collabId())},
			{"v", QJsonObject{{"muted", true}}}}});
		settle(300);
		a->setSolo(false);
		a->toggleSolo();
		QVERIFY2(b->isMuted(), "the shared mute applies when my solo ends");
		QVERIFY2(!a->isMuted(), "a was never muted");
		QVERIFY2(m_peer.nextForeignOp("track.set", 500).isEmpty(), "ending my solo sends nothing");

		delete a;
		delete b;
		m_peer.drain();
	}

	void testClipUndoIsPerUser()
	{
		m_peer.drain();
		auto journal = Engine::projectJournal();
		Track* track = m_clip->getTrack();

		// I delete a clip (as the Song Editor does: track checkpoint, then removal)
		auto clip = new MidiClip(dynamic_cast<InstrumentTrack*>(track));
		clip->movePosition(TimePos{1536});
		const collab_id_t noteId = clip->addNote(Note{TimePos{48}, TimePos{0}, 70}, false)->collabId();
		m_peer.nextForeignOp("clip.add");
		m_peer.drain();
		const collab_id_t clipId = clip->collabId();
		track->addJournalCheckPoint();
		delete clip;
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("clip.remove").value("id")), clipId);

		// Ctrl+Z brings it back with the same identity and its notes
		journal->undo();
		QVERIFY(findClip(clipId) != nullptr);
		QVERIFY(dynamic_cast<MidiClip*>(findClip(clipId))->findNote(noteId) != nullptr);
		const auto added = m_peer.nextForeignOp("clip.add");
		QVERIFY(added.value("xml").toString().contains(proto::idString(clipId)));

		// I move it, the peer moves it afterwards: my Ctrl+Z leaves the peer's position
		clip = dynamic_cast<MidiClip*>(findClip(clipId));
		clip->addJournalCheckPoint();
		clip->movePosition(TimePos{1920});
		m_peer.nextForeignOp("clip.set");
		m_peer.sendOps({QJsonObject{{"op", "clip.set"}, {"id", proto::idString(clipId)}, {"v", QJsonObject{{"pos", 2304}}}}});
		settle(300);
		m_peer.drain();
		journal->undo();
		QCOMPARE(clip->startPosition().getTicks(), 2304);

		// I add a clip, the peer adds a note into it: undoing my add must not delete the peer's work
		track->addJournalCheckPoint();
		auto mine = new MidiClip(dynamic_cast<InstrumentTrack*>(track));
		mine->movePosition(TimePos{3072});
		m_peer.nextForeignOp("clip.add");
		const collab_id_t mineId = mine->collabId();
		m_peer.sendOps({noteOp("note.add", mine, 0x00000000bbbb0001ULL,
			{{"key", 60}, {"pos", 0}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}})});
		settle(300);
		journal->undo();
		QVERIFY2(findClip(mineId) != nullptr, "a clip that others changed is not removed by my undo");
	}
	// ---- M2b: Pattern Editor ----

	void testPatternsSync()
	{
		m_peer.drain();
		auto song = Engine::getSong();
		auto store = Engine::patternStore();
		QCOMPARE(store->tracks().size(), std::size_t{1});
		Track* editorTrack = store->tracks().front();

		// A new pattern carries its content: its clip in every Pattern Editor track
		auto second = dynamic_cast<PatternTrack*>(Track::create(Track::Type::Pattern, song));
		QCOMPARE(second->patternIndex(), 1);
		auto op = m_peer.nextForeignOp("pattern.add");
		QCOMPARE(op.value("clips").toArray().size(), 1);
		const collab_id_t secondClipId = editorTrack->getClips()[1]->collabId();
		QVERIFY(op.value("clips").toArray().first().toObject().value("xml").toString().contains(proto::idString(secondClipId)));

		// Steps in the new pattern are shared notes
		dynamic_cast<MidiClip*>(editorTrack->getClips()[1])->addStepNote(3);
		QCOMPARE(m_peer.nextForeignOp("note.add").value("clip").toString(), proto::idString(secondClipId));

		// A pattern created by the peer: the local clips of the new pattern take the peer's ids
		const collab_id_t remotePattern = 0x00000000cccc0001ULL, remoteClip = 0x00000000cccc0002ULL;
		QDomDocument doc;
		QVERIFY(static_cast<bool>(doc.setContent(op.value("xml").toString())));
		doc.documentElement().setAttribute("cid", proto::idString(remotePattern));
		doc.documentElement().setAttribute("name", "Remote pattern");
		QDomDocument clipDoc;
		QVERIFY(static_cast<bool>(clipDoc.setContent(op.value("clips").toArray().first().toObject().value("xml").toString())));
		clipDoc.documentElement().setAttribute("cid", proto::idString(remoteClip));
		for (QDomElement n = clipDoc.documentElement().firstChildElement("note"); !n.isNull();
			n = clipDoc.documentElement().firstChildElement("note"))
		{
			clipDoc.documentElement().removeChild(n);
		}
		m_peer.sendOps({QJsonObject{{"op", "pattern.add"}, {"xml", toString(doc.documentElement())},
			{"clips", QJsonArray{QJsonObject{{"track", proto::idString(editorTrack->collabId())},
				{"xml", toString(clipDoc.documentElement())}}}}}});
		QTRY_VERIFY(findTrack(remotePattern) != nullptr);
		auto third = dynamic_cast<PatternTrack*>(findTrack(remotePattern));
		QCOMPARE(third->name(), QString{"Remote pattern"});
		QCOMPARE(editorTrack->getClips()[third->patternIndex()]->collabId(), remoteClip);
		m_peer.sendOps({noteOp("note.add", dynamic_cast<MidiClip*>(findClip(remoteClip)), 0x00000000cccc0003ULL,
			{{"key", 57}, {"pos", 0}, {"len", 12}, {"vol", 100}, {"pan", 0}, {"type", 1}})});
		QTRY_VERIFY(dynamic_cast<MidiClip*>(findClip(remoteClip))->findNote(0x00000000cccc0003ULL) != nullptr);

		// Reordering patterns: each pattern keeps its content, here and on the server
		auto contentOf = [&](PatternTrack* p) { return editorTrack->getClips()[p->patternIndex()]->collabId(); };
		QCOMPARE(contentOf(second), secondClipId);
		m_peer.drain();
		auto index = [&](Track* t) {
			return static_cast<int>(std::find(song->tracks().begin(), song->tracks().end(), t) - song->tracks().begin());
		};
		// what dragging "third" up by one does
		Track* above = song->tracks()[index(third) - 1];
		PatternTrack::swapPatternTracks(third, above);
		song->moveTrack(third, index(third) - 1);
		QCOMPARE(contentOf(second), secondClipId);
		QCOMPARE(contentOf(third), remoteClip);
		QVERIFY(!m_peer.nextForeignOp("track.order").isEmpty());
		QVERIFY2(snapshotContentOf(remotePattern) == remoteClip, "the server moved the content with the pattern");
		QVERIFY2(snapshotContentOf(second->collabId()) == secondClipId, "for both patterns");

		// ...and when the peer reorders
		QStringList order;
		for (Track* t : song->tracks()) { order.append(proto::idString(t->collabId())); }
		order.swapItemsAt(order.indexOf(proto::idString(third->collabId())),
			order.indexOf(proto::idString(second->collabId())));
		m_peer.sendOps({QJsonObject{{"op", "track.order"}, {"container", "song"},
			{"ids", QJsonArray::fromStringList(order)}}});
		QTRY_VERIFY(index(third) > index(second));
		QCOMPARE(contentOf(second), secondClipId);
		QCOMPARE(contentOf(third), remoteClip);

		// A new Pattern Editor track: one clip per pattern
		m_peer.drain();
		Track* hihat = Track::create(Track::Type::Instrument, store);
		op = m_peer.nextForeignOp("track.add");
		QCOMPARE(op.value("container").toString(), QString{"patternstore"});
		QVERIFY(op.value("xml").toString().contains(proto::idString(hihat->getClips()[2]->collabId())));
		hihat->setName("Hihat");
		QCOMPARE(m_peer.nextForeignOp("track.set").value("v").toObject(), (QJsonObject{{"name", "Hihat"}}));

		// More steps in a pattern clip: steps and length change, never the position
		dynamic_cast<MidiClip*>(hihat->getClips()[0])->setStepCount(32);
		const auto steps = m_peer.nextForeignOp("clip.set").value("v").toObject();
		QCOMPARE(steps.value("steps").toInt(), 32);
		QVERIFY(!steps.contains("pos"));

		// Removing patterns, locally and remotely
		const collab_id_t secondId = second->collabId();
		delete second;
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("pattern.remove").value("id")), secondId);
		QVERIFY(findClip(secondClipId) == nullptr);
		m_peer.sendOps({QJsonObject{{"op", "pattern.remove"}, {"id", proto::idString(remotePattern)}}});
		QTRY_VERIFY(findTrack(remotePattern) == nullptr);
		QVERIFY(findClip(remoteClip) == nullptr);
		QCOMPARE(editorTrack->getClips().size(), std::size_t{1});
		QVERIFY2(m_peer.nextForeignTx(500).isEmpty(), "no echo of the remote removal");
	}

	// ---- M4a: parameters ----

	//! A track created here and the same track rebuilt from its XML (as another client does) must number
	//! their parameters identically, or remote knob changes cannot find their knob
	void testParamPathsMatchAcrossClients()
	{
		auto song = Engine::getSong();
		auto original = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song));
		original->loadInstrument("tripleoscillator");
		QDomDocument doc;
		QDomElement parent = doc.createElement("clonedtrack");
		doc.appendChild(parent);
		original->saveState(doc, parent);
		Track* rebuilt = Track::create(parent.firstChildElement(), song);

		const QStringList a = CollabSession::describeParams(original);
		const QStringList b = CollabSession::describeParams(rebuilt);
		for (qsizetype i = 0; i < std::max(a.size(), b.size()); ++i)
		{
			const QString left = a.value(i), right = b.value(i);
			if (left != right) { qWarning("param %lld: [%s] vs [%s]", static_cast<long long>(i), qPrintable(left), qPrintable(right)); }
		}
		QVERIFY2(a.size() > 50, "the track's parameters are included");
		QCOMPARE(a, b);
		delete rebuilt;
		delete original;
	}

	void testParamsSync()
	{
		m_peer.drain();
		auto track = dynamic_cast<InstrumentTrack*>(m_clip->getTrack());
		const QString owner = proto::idString(track->collabId());

		// A knob turned by hand reaches the peer, identified by its path in the track
		track->volumeModel()->setValue(42);
		auto op = m_peer.nextForeignOp("param.set");
		QCOMPARE(op.value("owner").toString(), owner);
		QCOMPARE(op.value("v").toDouble(), 42.0);
		const QString volumePath = op.value("path").toString();
		QVERIFY(volumePath.startsWith("t:"));

		// The peer turns it: applied here, not echoed
		m_peer.sendOps({QJsonObject{{"op", "param.set"}, {"owner", owner}, {"path", volumePath}, {"v", 77}}});
		QTRY_COMPARE(track->volumeModel()->value(), 77.0f);
		QVERIFY2(m_peer.nextForeignOp("param.set", 400).isEmpty(), "no echo of remote parameter changes");

		// The tuning tab (microtuner) is part of the track too
		track->microtuner()->enabledModel()->setValue(true);
		QVERIFY(m_peer.nextForeignOp("param.set").value("path").toString().startsWith("mt:"));

		// Song settings: tempo and time signature, stored by the server too. LMMS keeps empty global
		// automation clips for them (as in the GUI); empty clips do not make them "automated".
		AutomationClip::globalAutomationClip(&Engine::getSong()->tempoModel())->clear();
		Engine::getSong()->tempoModel().setValue(150);
		op = m_peer.nextForeignOp("param.set");
		QCOMPARE(op.value("owner").toString(), QString{"song"});
		QCOMPARE(op.value("path").toString(), QString{"bpm"});
		m_peer.sendOps({QJsonObject{{"op", "param.set"}, {"owner", "song"}, {"path", "num"}, {"v", 3}}});
		QTRY_COMPARE(Engine::getSong()->getTimeSigModel().getNumerator(), 3);
		QDomDocument snapshot = serverSnapshot();
		QCOMPARE(snapshot.documentElement().firstChildElement("head").attribute("bpm"), QString{"150"});
		QCOMPARE(snapshot.documentElement().firstChildElement("head").attribute("timesig_numerator"), QString{"3"});

		// Values driven by automation are not the user's doing: not sent
		auto automationTrack = Track::create(Track::Type::Automation, Engine::getSong());
		auto automation = new AutomationClip(dynamic_cast<AutomationTrack*>(automationTrack));
		automation->addObject(track->panningModel());
		m_peer.drain();
		track->panningModel()->setValue(-30);
		QVERIFY2(m_peer.nextForeignOp("param.set", 500).isEmpty(), "automated parameters are not sent");
		delete automationTrack;

		// Once the knobs rest, the client of the latest change stores the track's settings on the server
		m_peer.drain();
		track->volumeModel()->setValue(55);
		m_peer.nextForeignOp("param.set");
		const auto state = m_peer.nextForeignOp("track.state", 3000);
		QVERIFY2(!state.isEmpty(), "settings sent once the knob rested");
		QVERIFY(state.value("xml").toString().startsWith("<instrumenttrack"));
		QDomElement stored;
		const QDomNodeList tracks = serverSnapshot().elementsByTagName("track");
		for (int i = 0; i < tracks.size(); ++i)
		{
			if (tracks.at(i).toElement().attribute("cid") == owner) { stored = tracks.at(i).toElement(); }
		}
		QCOMPARE(stored.firstChildElement("instrumenttrack").attribute("vol"), QString{"55"});

		// When the latest change is the peer's, storing is the peer's job, not ours
		m_peer.sendOps({QJsonObject{{"op", "param.set"}, {"owner", owner}, {"path", volumePath}, {"v", 60}}});
		QTRY_COMPARE(track->volumeModel()->value(), 60.0f);
		QVERIFY2(m_peer.nextForeignOp("track.state", 1800).isEmpty(), "only the author of the latest change stores");

		// Undo is per user for knobs too: my Ctrl+Z does not undo the peer's later turn
		auto journal = Engine::projectJournal();
		track->volumeModel()->addJournalCheckPoint();
		track->volumeModel()->setValue(20);
		m_peer.nextForeignOp("param.set");
		m_peer.sendOps({QJsonObject{{"op", "param.set"}, {"owner", owner}, {"path", volumePath}, {"v", 90}}});
		QTRY_COMPARE(track->volumeModel()->value(), 90.0f);
		m_peer.drain();
		journal->undo();
		QCOMPARE(track->volumeModel()->value(), 90.0f);
		// ...but it undoes my own turn when nobody touched it afterwards
		track->volumeModel()->addJournalCheckPoint();
		track->volumeModel()->setValue(33);
		m_peer.nextForeignOp("param.set");
		journal->undo();
		QCOMPARE(track->volumeModel()->value(), 90.0f);
		QCOMPARE(m_peer.nextForeignOp("param.set").value("v").toDouble(), 90.0);
	}

	// ---- M4b: instruments and effects ----

	void testInstrumentsAndEffects()
	{
		m_peer.drain();
		auto track = dynamic_cast<InstrumentTrack*>(m_clip->getTrack());
		const QString id = proto::idString(track->collabId());
		// Another instrument here (no plugins in this test: LMMS falls back to its dummy instrument, which is
		// still a new instrument object): the peer gets the whole instrument, the server the track's settings
		track->loadInstrument("kicker");
		const auto instrumentOp = m_peer.nextForeignOp("instrument.set");
		QCOMPARE(instrumentOp.value("track").toString(), id);
		QVERIFY(instrumentOp.value("xml").toString().startsWith("<instrument"));
		QVERIFY(!m_peer.nextForeignOp("track.state").isEmpty());

		// The peer sends the same plugin with new state: applied to the existing instrument, not echoed
		m_peer.drain();
		const Instrument* before = track->instrument();
		m_peer.sendOps({QJsonObject{{"op", "instrument.set"}, {"track", id},
			{"xml", R"(<instrument name="dummy"><dummyinstrument/></instrument>)"}}});
		settle(300);
		QCOMPARE(track->instrument(), before);
		QVERIFY2(m_peer.nextForeignOp("instrument.set", 500).isEmpty(), "no echo of a remote instrument state");

		// ...and another plugin: the instrument is replaced, still without echo
		m_peer.sendOps({QJsonObject{{"op", "instrument.set"}, {"track", id},
			{"xml", R"(<instrument name="tripleoscillator"><tripleoscillator/></instrument>)"}}});
		QTRY_VERIFY(track->instrument() != before);
		QVERIFY2(m_peer.nextForeignOp("instrument.set", 500).isEmpty(), "no echo of a remote instrument");

		// Effects added here reach the peer
		EffectChain* chain = track->audioBusHandle()->effects();
		chain->appendEffect(new DummyEffect(chain, QDomElement{}));
		const auto effectsOp = m_peer.nextForeignOp("effects.set");
		QVERIFY(effectsOp.value("xml").toString().startsWith("<fxchain"));
		QCOMPARE(effectsOp.value("track").toString(), id);

		// ...and the peer removing all effects removes them here, without echo
		m_peer.drain();
		m_peer.sendOps({QJsonObject{{"op", "effects.set"}, {"track", id},
			{"xml", R"(<fxchain enabled="1" numofeffects="0"/>)"}}});
		QTRY_COMPARE(chain->effects().size(), std::size_t{0});
		QVERIFY2(m_peer.nextForeignOp("effects.set", 500).isEmpty(), "no echo of remote effects");

		// Remote effects are applied one by one: a reorder or a removal keeps the other effect objects (and
		// so their views and open windows)
		auto effectXml = [](const QString& cid, const QString& name) {
			return QString{R"(<effect cid="%1" name="%2" wet="1" on="1"><key/></effect>)"}.arg(cid, name);
		};
		auto chainXml = [](const QStringList& effects) {
			return QString{R"(<fxchain enabled="1" numofeffects="%1">%2</fxchain>)"}.arg(effects.size()).arg(effects.join(""));
		};
		const QString a = effectXml("00000000000000a1", "ghostA"), b = effectXml("00000000000000b1", "ghostB");
		m_peer.sendOps({QJsonObject{{"op", "effects.set"}, {"track", id}, {"xml", chainXml({a, b})}}});
		QTRY_COMPARE(chain->effects().size(), std::size_t{2});
		Effect* first = chain->effects()[0];
		Effect* second = chain->effects()[1];
		m_peer.sendOps({QJsonObject{{"op", "effects.set"}, {"track", id}, {"xml", chainXml({b, a})}}});
		QTRY_COMPARE(chain->effects()[0], second);
		QCOMPARE(chain->effects()[1], first);
		m_peer.sendOps({QJsonObject{{"op", "effects.set"}, {"track", id}, {"xml", chainXml({b})}}});
		QTRY_COMPARE(chain->effects().size(), std::size_t{1});
		QCOMPARE(chain->effects()[0], second);
		QVERIFY2(m_peer.nextForeignOp("effects.set", 500).isEmpty(), "no echo of remote effects");

		// Moving an effect here sends the new order with the same ids
		m_peer.sendOps({QJsonObject{{"op", "effects.set"}, {"track", id}, {"xml", chainXml({a, b})}}});
		QTRY_COMPARE(chain->effects().size(), std::size_t{2});
		m_peer.drain();
		chain->moveUp(chain->effects()[1]);
		const QString moved = m_peer.nextForeignOp("effects.set").value("xml").toString();
		QVERIFY2(moved.indexOf("00000000000000b1") >= 0 && moved.indexOf("00000000000000b1") < moved.indexOf("00000000000000a1"),
			qPrintable(moved));

		// Deleting a track with effects: its chain is switched off while it is destroyed, which must not become
		// an undo step that serializes the half-destroyed track (crash)
		auto doomed = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		m_peer.nextForeignOp("track.add");
		EffectChain* doomedChain = doomed->audioBusHandle()->effects();
		doomedChain->appendEffect(new DummyEffect(doomedChain, QDomElement{}));
		m_peer.nextForeignOp("effects.set");
		delete doomed;
		QVERIFY(!m_peer.nextForeignOp("track.remove").isEmpty());
	}

	// ---- M5a: mixer ----

	void testMixerSync()
	{
		m_peer.drain();
		Mixer* mixer = Engine::mixer();
		auto track = dynamic_cast<InstrumentTrack*>(m_clip->getTrack());
		const QString trackId = proto::idString(track->collabId());

		// A channel added here reaches the peer (with its fields), and the server gets the whole mixer
		const int local = mixer->createChannel();
		mixer->mixerChannel(local)->m_name = "Drums";
		const QString localId = proto::idString(mixer->mixerChannel(local)->collabId());
		const auto add = m_peer.nextForeignOp("mixer.add");
		QCOMPARE(add.value("id").toString(), localId);
		QCOMPARE(m_peer.nextForeignOp("mixer.set").value("v").toObject().value("name").toString(), QString{"Drums"});
		QVERIFY(m_peer.nextForeignOp("mixer.state").value("xml").toString().contains(localId));

		// Assigning a track to it travels as the channel's id
		track->mixerChannelModel()->setRange(0, mixer->numChannels() - 1, 1);
		track->mixerChannelModel()->setValue(local);
		QCOMPARE(m_peer.nextForeignOp("track.set").value("v").toObject().value("channel").toString(), localId);

		// Its volume is a parameter owned by the channel
		mixer->mixerChannel(local)->m_volumeModel.setValue(0.5f);
		const auto volume = m_peer.nextForeignOp("param.set");
		QCOMPARE(volume.value("owner").toString(), localId);
		QCOMPARE(volume.value("path").toString(), QString{"c:0"});

		// A channel added by the peer, placed before ours, sending to ours, renamed, with its volume
		const QString remoteId = "00000000000c0001";
		m_peer.sendOps({QJsonObject{{"op", "mixer.add"}, {"id", remoteId}, {"index", 1}},
			QJsonObject{{"op", "mixer.set"}, {"id", remoteId}, {"v", QJsonObject{{"name", "Bass"}}}},
			QJsonObject{{"op", "mixer.order"}, {"ids", QJsonArray{remoteId, localId}}},
			QJsonObject{{"op", "mixer.send"}, {"from", remoteId}, {"to", localId}, {"on", true}}});
		QTRY_COMPARE(mixerChannelIndex(idFromString(remoteId)), 1);
		QCOMPARE(mixerChannelIndex(idFromString(localId)), 2);
		QCOMPARE(mixer->mixerChannel(1)->m_name, QString{"Bass"});
		QVERIFY(mixer->channelSendModel(1, 2) != nullptr);
		QCOMPARE(track->mixerChannelModel()->value(), 2); // the track followed its channel
		m_peer.sendOps({QJsonObject{{"op", "param.set"}, {"owner", remoteId}, {"path", "c:0"}, {"v", 0.25},
			{"k", "lmms::FloatModel/0/2"}}});
		QTRY_COMPARE(mixer->mixerChannel(1)->m_volumeModel.value(), 0.25f);
		QVERIFY2(m_peer.nextForeignOp("mixer.add", 500).isEmpty(), "no echo of remote mixer changes");
		// As a real client does, the peer stores its mixer on the server (here: the same as ours now)
		QDomDocument mixerDoc;
		QDomElement mixerParent = mixerDoc.createElement("collab");
		mixerDoc.appendChild(mixerParent);
		mixer->saveState(mixerDoc, mixerParent);
		m_peer.sendOps({QJsonObject{{"op", "mixer.state"}, {"xml", toString(mixerParent.firstChildElement())}}});

		// The peer moves our track to its channel, then removes that channel: the track goes to the master
		m_peer.sendOps({QJsonObject{{"op", "track.set"}, {"id", trackId}, {"v", QJsonObject{{"channel", remoteId}}}}});
		QTRY_COMPARE(track->mixerChannelModel()->value(), 1);
		m_peer.sendOps({QJsonObject{{"op", "mixer.remove"}, {"id", remoteId}}});
		QTRY_COMPARE(mixerChannelIndex(idFromString(remoteId)), -1);
		QCOMPARE(track->mixerChannelModel()->value(), 0);

		// Solo stays private: soloing mutes the other channels here, but no mute is sent
		m_peer.drain();
		mixer->mixerChannel(0)->m_soloModel.setValue(true);
		mixer->toggledSolo();
		QVERIFY(mixer->mixerChannel(1)->m_muteModel.value());
		QVERIFY2(m_peer.nextForeignOp("mixer.set", 500).isEmpty(), "a solo is not shared");
		mixer->mixerChannel(0)->m_soloModel.setValue(false);
		mixer->toggledSolo();

		// The server's copy: our channel with its id and name, the track on the master channel
		QTRY_VERIFY(serverSnapshot().toString().contains(QString{"cid=\"%1\""}.arg(localId)));
		const QDomDocument snapshot = serverSnapshot();
		const QDomNodeList channels = snapshot.elementsByTagName("mixerchannel");
		QCOMPARE(channels.size(), 2);
		QCOMPARE(channels.at(1).toElement().attribute("name"), QString{"Drums"});
		QDomElement settings;
		const QDomNodeList tracks = snapshot.elementsByTagName("track");
		for (int i = 0; i < tracks.size(); ++i)
		{
			if (tracks.at(i).toElement().attribute("cid") == trackId)
			{
				settings = tracks.at(i).firstChildElement("instrumenttrack");
			}
		}
		QCOMPARE(settings.attribute("mixch"), QString{"0"});

		// Removing our channel reaches the peer
		m_peer.drain();
		mixer->deleteChannel(mixerChannelIndex(idFromString(localId)));
		QCOMPARE(m_peer.nextForeignOp("mixer.remove").value("id").toString(), localId);
	}

	// ---- M5b: automation ----

	void testAutomationSync()
	{
		m_peer.drain();
		auto instrument = dynamic_cast<InstrumentTrack*>(m_clip->getTrack());
		const QString instrumentId = proto::idString(instrument->collabId());
		QString volumePath;
		for (const QString& line : CollabSession::describeParams(instrument))
		{
			if (line.endsWith(" Volume") && line.startsWith("t:")) { volumePath = line.section(' ', 0, 0); break; }
		}
		QVERIFY(!volumePath.isEmpty());

		// An automation track made here: the peer gets its clip with the automated parameter by name
		auto track = Track::create(Track::Type::Automation, Engine::getSong());
		auto clip = dynamic_cast<AutomationClip*>(track->createClip(TimePos{0}));
		clip->addObject(instrument->volumeModel());
		clip->putValue(TimePos{0}, 20, false);
		const auto add = m_peer.nextForeignOp("track.add");
		const QString xml = add.value("xml").toString();
		QVERIFY2(xml.contains(QString{"owner=\"%1\""}.arg(instrumentId)) && xml.contains(QString{"path=\"%1\""}.arg(volumePath)),
			qPrintable(xml));

		// Drawing sends the whole content
		m_peer.drain();
		clip->putValue(TimePos{96}, 80, false);
		emit clip->dataChanged();
		const auto set = m_peer.nextForeignOp("automation.set");
		QCOMPARE(set.value("clip").toString(), proto::idString(clip->collabId()));
		QCOMPARE(set.value("nodes").toArray().size(), 2);
		QCOMPARE(set.value("objects").toArray().first().toObject().value("path").toString(), volumePath);

		// The peer's content: other nodes, now automating the tempo; not echoed
		m_peer.sendOps({QJsonObject{{"op", "automation.set"}, {"clip", proto::idString(clip->collabId())},
			{"prog", 1}, {"tens", 1}, {"nodes", QJsonArray{QJsonArray{0, 120, 120, 0, 0, 0}, QJsonArray{192, 140, 140, 0, 0, 0}}},
			{"objects", QJsonArray{QJsonObject{{"owner", "song"}, {"path", "bpm"}}}}}});
		QTRY_COMPARE(clip->getTimeMap().size(), 2);
		QTRY_VERIFY(clip->getTimeMap().contains(192));
		QCOMPARE(clip->objects().size(), std::size_t{1});
		QCOMPARE(clip->firstObject(), static_cast<const AutomatableModel*>(&Engine::getSong()->tempoModel()));
		QVERIFY2(m_peer.nextForeignOp("automation.set", 500).isEmpty(), "no echo of remote automation");

		// An automation track made by the peer, automating our instrument's volume
		const QString remoteTrack = "00000000000f0001", remoteClip = "00000000000f0002";
		m_peer.sendOps({QJsonObject{{"op", "track.add"}, {"container", "song"}, {"index", -1},
			{"xml", QString{R"(<track type="5" name="Auto" cid="%1" muted="0" solo="0"><automationtrack/>)"
				R"(<automationclip cid="%2" pos="0" len="192" name="" prog="0" tens="1" mute="0">)"
				R"(<time pos="0" value="50" outValue="50"/><object owner="%3" path="%4"/></automationclip></track>)"}
				.arg(remoteTrack, remoteClip, instrumentId, volumePath)}}});
		QTRY_VERIFY(findTrack(idFromString(remoteTrack)) != nullptr);
		auto remote = dynamic_cast<AutomationClip*>(findTrack(idFromString(remoteTrack))->getClips().front());
		QCOMPARE(remote->firstObject(), static_cast<const AutomatableModel*>(instrument->volumeModel()));

		// Ctrl+Z of my own drawing restores my content and sends it
		auto journal = Engine::projectJournal();
		journal->setJournalling(true);
		m_peer.drain();
		clip->addJournalCheckPoint();
		clip->putValue(TimePos{384}, 150, false);
		emit clip->dataChanged();
		QVERIFY(!m_peer.nextForeignOp("automation.set").isEmpty());
		journal->undo();
		QTRY_VERIFY(!clip->getTimeMap().contains(384));
		QVERIFY(clip->getTimeMap().contains(192));
		QVERIFY(!m_peer.nextForeignOp("automation.set").isEmpty());
		journal->setJournalling(false);
	}

	// ---- M5c: controllers ----

	void testControllersSync()
	{
		m_peer.drain();
		Song* song = Engine::getSong();
		auto instrument = dynamic_cast<InstrumentTrack*>(m_clip->getTrack());
		const QString instrumentId = proto::idString(instrument->collabId());
		auto pathOf = [&](const QString& name) {
			for (const QString& line : CollabSession::describeParams(instrument))
			{
				if (line.endsWith(" " + name) && line.startsWith("t:")) { return line.section(' ', 0, 0); }
			}
			return QString{};
		};

		// An LFO added here reaches the peer, with the whole rack for the server
		auto lfo = new LfoController(song);
		song->addController(lfo);
		const QString lfoId = proto::idString(lfo->collabId());
		QCOMPARE(m_peer.nextForeignOp("controller.add").value("id").toString(), lfoId);
		QVERIFY(m_peer.nextForeignOp("controllers.state").value("xml").toString().contains(lfoId));

		// Renamed, a knob turned, a parameter connected to it
		lfo->setName("Wobble");
		QCOMPARE(m_peer.nextForeignOp("controller.set").value("v").toObject().value("name").toString(), QString{"Wobble"});
		auto knob = lfo->findChildren<FloatModel*>().front();
		knob->setValue(knob->value() == knob->maxValue() ? knob->minValue() : knob->maxValue());
		const auto knobOp = m_peer.nextForeignOp("param.set");
		QCOMPARE(knobOp.value("owner").toString(), lfoId);
		QVERIFY(knobOp.value("path").toString().startsWith("k:"));
		instrument->volumeModel()->setControllerConnection(new ControllerConnection(lfo));
		const auto link = m_peer.nextForeignOp("param.link");
		QCOMPARE(link.value("owner").toString(), instrumentId);
		QCOMPARE(link.value("path").toString(), pathOf("Volume"));
		QCOMPARE(link.value("controller").toString(), lfoId);

		// An LFO added by the peer, and our panning connected to it, then disconnected
		QDomDocument doc;
		QDomElement parent = doc.createElement("collab");
		doc.appendChild(parent);
		lfo->saveState(doc, parent);
		const QString remoteId = "0000000000200001";
		QDomElement remoteXml = parent.firstChildElement();
		remoteXml.setAttribute("cid", remoteId);
		remoteXml.setAttribute("name", "Remote LFO");
		m_peer.sendOps({QJsonObject{{"op", "controller.add"}, {"id", remoteId}, {"xml", toString(remoteXml)}}});
		QTRY_VERIFY(findController(remoteId) != nullptr);
		QCOMPARE(findController(remoteId)->name(), QString{"Remote LFO"});
		m_peer.sendOps({QJsonObject{{"op", "param.link"}, {"owner", instrumentId}, {"path", pathOf("Panning")},
			{"controller", remoteId}}});
		QTRY_VERIFY(instrument->panningModel()->controllerConnection() != nullptr);
		QCOMPARE(instrument->panningModel()->controllerConnection()->getController(), findController(remoteId));
		QVERIFY2(m_peer.nextForeignOp("param.link", 500).isEmpty(), "no echo of a remote connection");
		m_peer.sendOps({QJsonObject{{"op", "param.link"}, {"owner", instrumentId}, {"path", pathOf("Panning")},
			{"controller", ""}}});
		QTRY_VERIFY(instrument->panningModel()->controllerConnection() == nullptr);

		// The peer removes its LFO; we disconnect and remove ours
		m_peer.sendOps({QJsonObject{{"op", "controller.remove"}, {"id", remoteId}}});
		QTRY_VERIFY(findController(remoteId) == nullptr);
		m_peer.drain();
		delete instrument->volumeModel()->controllerConnection();
		instrument->volumeModel()->setControllerConnection(nullptr);
		QCOMPARE(m_peer.nextForeignOp("param.link").value("controller").toString(), QString{});
		song->removeController(lfo);
		QCOMPARE(m_peer.nextForeignOp("controller.remove").value("id").toString(), lfoId);
	}

	// ---- M6a: shared files ----

	//! A small valid WAV file (16-bit mono), different for each seed
	static QByteArray wav(int seed)
	{
		QByteArray data;
		auto u32 = [&data](quint32 v) { for (int i = 0; i < 4; ++i) { data.append(static_cast<char>((v >> (8 * i)) & 0xff)); } };
		auto u16 = [&data](quint16 v) { data.append(static_cast<char>(v & 0xff)); data.append(static_cast<char>(v >> 8)); };
		const int samples = 2000;
		data.append("RIFF"); u32(36 + samples * 2); data.append("WAVEfmt "); u32(16); u16(1); u16(1); u32(44100);
		u32(88200); u16(2); u16(16); data.append("data"); u32(samples * 2);
		for (int i = 0; i < samples; ++i) { u16(static_cast<quint16>((i * seed * 37) & 0x7fff)); }
		return data;
	}

	void testSharedFiles()
	{
		m_peer.drain();
		const QString libraryDir = m_dataDir.filePath("workspace/collab/session-test/Project files/");
		QCOMPARE(PathUtil::sharedLocation(), QDir::cleanPath(libraryDir) + "/");

		// A sample only this computer has: shared first (no GUI: without asking), then the new track names it
		const QByteArray kick = wav(3);
		const QString kickFile = m_dataDir.filePath("my-kick.wav");
		QFile out{kickFile};
		QVERIFY(out.open(QIODevice::WriteOnly));
		out.write(kick);
		out.close();
		auto track = dynamic_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong()));
		auto clip = dynamic_cast<SampleClip*>(track->createClip(TimePos{0}));
		clip->setSampleFile(kickFile);
		const auto added = m_peer.nextForeignOp("library.add", 5000);
		QCOMPARE(added.value("path").toString(), QString{"my-kick.wav"});
		const QString trackXml = m_peer.nextForeignOp("track.add").value("xml").toString();
		QVERIFY2(trackXml.contains("shared:my-kick.wav") && !trackXml.contains(kickFile), qPrintable(trackXml));
		QCOMPARE(clip->sampleFile(), QString{"shared:my-kick.wav"});
		QCOMPARE(m_peer.download(added.value("hash").toString()), kick);

		// A file the peer shares is downloaded here
		const QByteArray snare = wav(5);
		const QString snarePath = m_peer.upload(snare, "snare.wav");
		QCOMPARE(snarePath, QString{"snare.wav"});
		const QString snareHash = QString::fromLatin1(QCryptographicHash::hash(snare, QCryptographicHash::Sha256).toHex());
		m_peer.sendOps({QJsonObject{{"op", "library.add"}, {"path", snarePath}, {"hash", snareHash}, {"size", snare.size()}}});
		QTRY_COMPARE_WITH_TIMEOUT(QFileInfo{libraryDir + "snare.wav"}.size(), static_cast<qint64>(snare.size()), 5000);
		QFile in{libraryDir + "snare.wav"};
		QVERIFY(in.open(QIODevice::ReadOnly));
		QCOMPARE(in.readAll(), snare);

		// A local file with the same content as a shared one is simply the shared one: nothing uploaded
		m_peer.drain();
		const QString copy = m_dataDir.filePath("same-snare.wav");
		QFile copyOut{copy};
		QVERIFY(copyOut.open(QIODevice::WriteOnly));
		copyOut.write(snare);
		copyOut.close();
		auto second = dynamic_cast<SampleClip*>(track->createClip(TimePos{192}));
		second->setSampleFile(copy);
		const QString clipXml = m_peer.nextForeignOp("clip.add").value("xml").toString();
		QVERIFY2(clipXml.contains("shared:snare.wav"), qPrintable(clipXml));
		QVERIFY2(m_peer.nextForeignOp("library.add", 500).isEmpty(), "the same content is not shared twice");

		// Joining again without the shared files: they are downloaded before the project is loaded
		auto session = CollabSession::instance();
		session->disconnectFromServer();
		QDir{libraryDir}.removeRecursively();
		session->connectToServer("127.0.0.1", m_port, "Adri", "session-test", CollabSession::JoinMode::Open);
		QTRY_COMPARE_WITH_TIMEOUT(session->state(), CollabSession::State::Live, 5000);
		QCOMPARE(QFileInfo{libraryDir + "snare.wav"}.size(), static_cast<qint64>(snare.size()));
		QCOMPARE(QFileInfo{libraryDir + "my-kick.wav"}.size(), static_cast<qint64>(kick.size()));
		bool kickLoaded = false;
		for (Track* t : Engine::getSong()->tracks())
		{
			for (Clip* c : t->getClips())
			{
				auto sample = dynamic_cast<SampleClip*>(c);
				if (sample && sample->sampleFile() == "shared:my-kick.wav") { kickLoaded = true; }
			}
		}
		QVERIFY2(kickLoaded, "the project's clips use the downloaded file");
	}

	void testEditsWhileJoining()
	{
		// While this client downloads the project's files to join, the peer goes on: a new track, a new shared
		// file and a clip using it. All of it is applied once the download is done, in order.
		auto session = CollabSession::instance();
		const QString libraryDir = m_dataDir.filePath("workspace/collab/session-test/Project files/");
		session->disconnectFromServer();
		QByteArray big(40 * 1024 * 1024, '\0');
		for (int i = 0; i < big.size(); i += 4096) { big[i] = static_cast<char>(i / 4096); }
		const QByteArray header = wav(7).left(44);
		big.replace(0, 44, header);
		const QString bigHash = QString::fromLatin1(QCryptographicHash::hash(big, QCryptographicHash::Sha256).toHex());
		QCOMPARE(m_peer.upload(big, "big.wav"), QString{"big.wav"});
		m_peer.sendOps({QJsonObject{{"op", "library.add"}, {"path", "big.wav"}, {"hash", bigHash}, {"size", big.size()}}});
		settle(300);
		QDir{libraryDir}.removeRecursively();

		session->connectToServer("127.0.0.1", m_port, "Adri", "session-test", CollabSession::JoinMode::Open);
		// Wait until the download has started, then edit without letting it go on meanwhile
		QElapsedTimer timer;
		timer.start();
		auto downloading = [&] { return !QDir{libraryDir}.entryList({".part-*"}, QDir::Files | QDir::Hidden).isEmpty(); };
		while (!downloading() && timer.elapsed() < 5000) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); }
		QVERIFY2(downloading(), "the download started");
		QVERIFY(session->state() != CollabSession::State::Live);

		const QByteArray late = wav(9);
		const QString lateHash = QString::fromLatin1(QCryptographicHash::hash(late, QCryptographicHash::Sha256).toHex());
		QCOMPARE(m_peer.upload(late, "late.wav"), QString{"late.wav"});
		const QString trackId = "0000000000400001";
		m_peer.sendOps({QJsonObject{{"op", "library.add"}, {"path", "late.wav"}, {"hash", lateHash}, {"size", late.size()}},
			QJsonObject{{"op", "track.add"}, {"container", "song"}, {"index", -1}, {"xml",
				QString{R"(<track type="2" name="Late" cid="%1" muted="0" solo="0"><sampletrack vol="100" pan="0"/>)"
					R"(<sampleclip cid="0000000000400002" pos="0" len="192" muted="0" src="shared:late.wav" off="0"/></track>)"}
					.arg(trackId)}}});

		QTRY_COMPARE_WITH_TIMEOUT(session->state(), CollabSession::State::Live, 15000);
		QCOMPARE(QFileInfo{libraryDir + "big.wav"}.size(), static_cast<qint64>(big.size()));
		QTRY_VERIFY(findTrack(idFromString(trackId)) != nullptr);
		QTRY_COMPARE_WITH_TIMEOUT(QFileInfo{libraryDir + "late.wav"}.size(), static_cast<qint64>(late.size()), 5000);
		auto clip = dynamic_cast<SampleClip*>(findTrack(idFromString(trackId))->getClips().front());
		QCOMPARE(clip->sampleFile(), QString{"shared:late.wav"});
		QVERIFY2(clip->sample().sampleSize() > 1000, "the clip plays the real file, not the placeholder");
	}

	// ---- M7a: save status, project list ----

	void testSaveStatus()
	{
		auto session = CollabSession::instance();
		QCOMPARE(session->state(), CollabSession::State::Live);
		// A change is sent, then the server writes it to disk; "save now" does it at once
		// (the project was loaded again by the tests before: a new track rather than m_clip)
		Track::create(Track::Type::Instrument, Engine::getSong());
		QTRY_COMPARE_WITH_TIMEOUT(session->syncStatus(), CollabSession::SyncStatus::Saving, 3000);
		session->saveNow();
		QTRY_COMPARE_WITH_TIMEOUT(session->syncStatus(), CollabSession::SyncStatus::Saved, 3000);
		QVERIFY(session->lastSaved().isValid());

		// The server lists its projects, with who is connected
		FakePeer lister;
		QVERIFY(lister.connectTo(m_port));
		lister.send({{"t", "list"}});
		const QJsonArray projects = lister.next("projects").value("projects").toArray();
		bool found = false;
		for (const QJsonValue& p : projects)
		{
			if (p.toObject().value("name").toString() == "session-test")
			{
				found = true;
				QVERIFY(p.toObject().value("users").toInt() >= 2);
			}
		}
		QVERIFY(found);
	}

	// ---- M7b: reconnecting ----

	void testReconnect()
	{
		auto session = CollabSession::instance();
		QVERIFY(session->waitUntilSaved(3000));
		// The server goes away: the session keeps trying, and is back once the server is
		m_server.kill();
		m_server.waitForFinished(3000);
		QTRY_COMPARE_WITH_TIMEOUT(session->state(), CollabSession::State::Reconnecting, 5000);
		// Meanwhile a clip is moved (LMMS does not mark that as "modified"): it counts as an offline change, and
		// the project continues as it is on the server
		const auto firstClip = [] () -> std::pair<int, Clip*> {
			const auto& tracks = Engine::getSong()->tracks();
			for (int t = 0; t < static_cast<int>(tracks.size()); ++t)
			{
				if (tracks[t]->numOfClips() > 0) { return {t, tracks[t]->getClip(0)}; }
			}
			return {-1, nullptr};
		};
		auto [trackIndex, clip] = firstClip();
		QVERIFY(clip);
		const TimePos position = clip->startPosition();
		clip->movePosition(position + TimePos::ticksPerBar());
		m_server.start(COLLAB_SERVER_EXE, {"--port", QString::number(m_port), "--data", m_dataDir.path()});
		QVERIFY(m_server.waitForStarted(5000));
		QTRY_COMPARE_WITH_TIMEOUT(session->state(), CollabSession::State::Live, 15000);
		QCOMPARE(session->projectName(), QString{"session-test"});
		QVERIFY(session->hadOfflineChanges());
		auto [trackAfter, clipAfter] = firstClip();
		QCOMPARE(trackAfter, trackIndex);
		QVERIFY(clipAfter);
		QCOMPARE(clipAfter->startPosition().getTicks(), position.getTicks());
		// "Disconnect" ends it for good: no reconnecting afterwards
		session->leave();
		QCOMPARE(session->state(), CollabSession::State::Disconnected);
		settle(1500);
		QCOMPARE(session->state(), CollabSession::State::Disconnected);
	}

private:
	QString m_newTrackXml;

	QDomDocument serverSnapshot()
	{
		FakePeer watcher;
		QDomDocument doc;
		if (!watcher.connectTo(m_port)) { return doc; }
		watcher.send({{"t", "open"}, {"project", "session-test"}});
		doc.setContent(watcher.next("joined").value("mmp").toString());
		return doc;
	}

	static QString toString(const QDomElement& e)
	{
		QString text;
		QTextStream ts{&text};
		e.save(ts, 0);
		return text;
	}

	//! Clip id of pattern @p patternId's content in the first Pattern Editor track of a fresh server snapshot
	collab_id_t snapshotContentOf(collab_id_t patternId)
	{
		FakePeer watcher;
		if (!watcher.connectTo(m_port)) { return 0; }
		watcher.send({{"t", "open"}, {"project", "session-test"}});
		QDomDocument doc;
		if (!doc.setContent(watcher.next("joined").value("mmp").toString())) { return 0; }
		QDomElement song = doc.documentElement().firstChildElement("song").firstChildElement("trackcontainer");
		int rank = 0, patternRank = -1;
		for (QDomElement t = song.firstChildElement("track"); !t.isNull(); t = t.nextSiblingElement("track"))
		{
			if (t.attribute("type") != "1") { continue; }
			if (proto::parseId(t.attribute("cid")) == patternId) { patternRank = rank; }
			++rank;
		}
		const QDomNodeList containers = doc.elementsByTagName("trackcontainer");
		for (int i = 0; i < containers.size(); ++i)
		{
			const QDomElement c = containers.at(i).toElement();
			if (c.attribute("type") != "patternstore") { continue; }
			int k = 0;
			for (QDomElement clip = c.firstChildElement("track").firstChildElement("midiclip"); !clip.isNull();
				clip = clip.nextSiblingElement("midiclip"), ++k)
			{
				if (k == patternRank) { return proto::parseId(clip.attribute("cid")); }
			}
		}
		return 0;
	}

	static Track* findTrack(collab_id_t id)
	{
		return const_cast<Track*>(static_cast<const Track*>(findOwner(IdScope::Track, id)));
	}
	static Clip* findClip(collab_id_t id)
	{
		return const_cast<Clip*>(static_cast<const Clip*>(findOwner(IdScope::Clip, id)));
	}
};

QTEST_GUILESS_MAIN(CollabSessionTest)
#include "CollabSessionTest.moc"
